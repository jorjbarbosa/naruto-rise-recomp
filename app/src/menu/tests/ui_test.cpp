// GPU check of the menus' UI (menu/ui.h) in the native mode: RmlUi with the
// embedded fonts and documents, its render interface over the UI layer's
// immediate drawing, delivered through a dispatcher as the Swaps do. The frame
// rate document over a green image must show its navy panel (straight alpha
// over the image), its orange left border and text; hidden, the image alone.
// The setup screen must show its centered identity, focus and state, follow the
// keyboard and the mouse, and come back on a new engine after the UI screen's
// own engine (gpu/ui/screen.h) presented it on the same window.
#include "gpu/backend/dispatcher.h"
#include "gpu/backend/tests/test_backend.h"
#include "gpu/ui/immediate_drawer.h"
#include "gpu/ui/layer.h"
#include "gpu/ui/screen.h"
#include "menu/ui.h"
#include "menu/assets.h"
#include "menu/settings.h"
#include "menu/dlc.h"
#include "menu/content_manager.h"
#include <rex/filesystem/devices/stfs_xbox.h>
#include <rex/system/xam/content_manager.h>
#include <toml++/toml.hpp>
#include <RmlUi/Core.h>
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <rex/ui/presenter.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/cvar.h>

// Native settings are normally registered by the product's graphics host.
REXCVAR_DEFINE_INT32(frame_rate, 30, "test", "Setup application regression");
REXCVAR_DEFINE_STRING(native_backend, "d3d12", "test", "Setup override regression");
REXCVAR_DEFINE_BOOL(skip_intro_videos, false, "test", "Setup override regression");

using namespace gpu;

namespace gpu::sampler {
void RegisterThread(const char*) {}  // The sampler is not linked here.
}  // namespace gpu::sampler

namespace {
constexpr uint32_t kWidth = 640, kHeight = 360, kSurface = 0x1234;
std::array<int, 3> Pixel(const std::vector<uint8_t>& bgra, uint32_t x, uint32_t y) {
  const uint8_t* p = bgra.data() + (size_t(y) * kWidth + x) * 4;
  return {p[2], p[1], p[0]};
}
bool Near(const std::array<int, 3>& got, const std::array<int, 3>& want, int tolerance) {
  for (int c = 0; c < 3; ++c)
    if (std::abs(got[c] - want[c]) > tolerance) return false;
  return true;
}
std::string Json(const std::array<int, 3>& p) {
  return "[" + std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]) + "]";
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  // Only the SDL virtual fixtures participate in this executable. Never
  // assign or rumble a controller connected to the user's desktop.
  for(const char* hint:{SDL_HINT_JOYSTICK_HIDAPI,SDL_HINT_JOYSTICK_DIRECTINPUT,SDL_HINT_JOYSTICK_RAWINPUT,SDL_HINT_JOYSTICK_WGI,SDL_HINT_JOYSTICK_GAMEINPUT,SDL_HINT_XINPUT_ENABLED})
    SDL_SetHintWithPriority(hint,"0",SDL_HINT_OVERRIDE);
  std::ofstream output(argv[1]);
  if (!output) return 3;
  bool passed = true;
  auto check = [&](const std::string& kind, bool ok, const std::string& evidence) {
    passed &= ok;
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << (ok ? "true" : "false")
           << ",\"evidence\":" << (evidence.empty() ? "null" : evidence) << "}\n";
    output.flush();
    return ok;
  };
  WNDCLASSW cls{};
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"NativeMenuTest";
  RegisterClassW(&cls);
  HWND window = CreateWindowW(cls.lpszClassName, L"Menu UI test", WS_POPUP, 0, 0, int(kWidth), int(kHeight),
                              nullptr, nullptr, cls.hInstance, nullptr);
  if (!check("window", window != nullptr, "{}")) return 4;

  // An engine on the window over a green image, as the game's; one at a time,
  // as the UI screen before the game and the game's engine are.
  std::string evidence;
  const auto open = [&](const std::string& name) {
    auto engine = std::make_unique<Dispatcher>(true);
    if (!check(name + "initialize", engine->Initialize(evidence, TestRenderMode()), evidence)) return engine;
    PresentMode present;
    present.vsync = false;
    check(name + "attach", engine->Run([&](Engine& e, std::string& out) {
      return e.AttachWindow(platform::WindowHandles(window), out, present) && e.CreateSurface(SurfaceDesc{kSurface, kWidth, kHeight}, out);
    }, evidence), evidence);
    ClearRequest clear;
    clear.color_keys[0] = kSurface;
    clear.clear_color = true;
    clear.color = {0, 1, 0, 1};
    check(name + "green", engine->Run([&](Engine& e, std::string& out) {
      uint64_t fence = 0;
      return e.ClearSurfaces(clear, fence, out);
    }, evidence), evidence);
    return engine;
  };
  const auto close = [&](const std::string& name, std::unique_ptr<Dispatcher>& engine) {
    check(name + "release", engine->Run([](Engine& e, std::string& out) {
      return e.ReleaseRemaining(out) && e.DetachWindow(out);
    }, evidence), evidence);
    check(name + "shutdown", engine->Shutdown(evidence), evidence);
    engine.reset();
  };
  std::unique_ptr<Dispatcher> engine = open("");

  // The setup screen at 1280 x 720 (one dp a pixel): the document over the
  // test engine, its keyboard and mouse, then the UI screen's own engine on a
  // second window, and the hand-over back to the first engine.
  const auto setup = [&](narutorise::menu::Ui& menu, ui::Layer& layer, std::unique_ptr<Dispatcher>& engine,
                         HWND first) {
    constexpr uint32_t kSetupWidth = 1280, kSetupHeight = 720;
    SetWindowPos(first, nullptr, 0, 0, int(kSetupWidth), int(kSetupHeight), SWP_NOZORDER | SWP_NOACTIVATE);
    rex::ui::AppUIDrawContext context(kSetupWidth, kSetupHeight);
    const auto at = [&](const std::vector<uint8_t>& bgra, uint32_t x, uint32_t y) {
      const uint8_t* p = bgra.data() + (size_t(y) * kSetupWidth + x) * 4;
      return std::array<int, 3>{p[2], p[1], p[0]};
    };
    const auto frame = [&](const std::string& kind, std::vector<uint8_t>& pixels) {
      menu.Draw(context);
      check(kind + "-deliver", layer.Deliver(*engine), "{}");
      uint32_t w = 0, h = 0;
      return check(kind + "-present", engine->Run([&](Engine& e, std::string& out) {
        uint32_t slot = 0;
        return e.StageFront(kSurface, slot, out) && e.Present(slot, true, pixels, w, h, out);
      }, evidence) && w == kSetupWidth && h == kSetupHeight, evidence);
    };
    const std::array<int, 3> navy{14, 18, 32}, orange{242, 112, 31};
    int played = 0, quits = 0, dlcs = 0;
    narutorise::menu::SetupScreen::State state;
    state.reason = narutorise::menu::SetupReason::kRequested;
    state.game.status = narutorise::game_image::Status::kReady;
    state.game.disc_image = true;
    state.version = "0.0.0-test";
    narutorise::menu::SetupScreen::Actions actions;
    actions.play = [&] { ++played; };
    actions.quit = [&] { ++quits; };
    actions.dlc = [&] { ++dlcs; };
    check("setup-show", menu.ShowSetup(state, actions) && menu.Shown(), "{}");
    std::vector<uint8_t> shown;
    if (frame("setup", shown)) {
      std::ofstream(std::string(argv[1]) + ".setup-1280x720.bgra", std::ios::binary)
          .write(reinterpret_cast<const char*>(shown.data()), std::streamsize(shown.size()));
      const auto background = at(shown, 20, 20), left = at(shown, 200, 250), right = at(shown, 1080, 250),
                 play = at(shown, 500, 353), settings = at(shown, 500, 407);
      check("setup-background", Near(background, {8,15,27}, 4), Json(background));
      check("setup-centered-background", Near(left, right, 3), "{\"left\":"+Json(left)+",\"right\":"+Json(right)+"}");
      check("setup-focus", Near(play, {245,138,53}, 4), Json(play));
      check("setup-unfocused", !Near(settings, {245,138,53}, 40), Json(settings));
      unsigned title_pixels = 0;
      for (uint32_t y=120;y<225;++y) for(uint32_t x=450;x<830;++x) {
        const auto p=at(shown,x,y); title_pixels += p[0]>150 && p[1]>150 && p[2]>150;
      }
      check("setup-text-title-without-logo",title_pixels>500,"{\"pixels\":"+std::to_string(title_pixels)+"}");
    }
    // Down moves the focus to Settings; Enter on Quit runs it.
    const auto key = [&](rex::ui::VirtualKey vk) {
      rex::ui::KeyEvent down(nullptr, vk, 1, false, false, false, false, false);
      rex::ui::KeyEvent up(nullptr, vk, 1, true, false, false, false, false);
      menu.OnKeyDown(down);
      menu.OnKeyUp(up);
      return down.is_handled();
    };
    check("setup-key-taken", key(rex::ui::VirtualKey::kDown), "{}");
    std::vector<uint8_t> moved;
    if (frame("setup-down", moved)) {
      const auto play = at(moved, 500, 353), settings = at(moved, 500, 407);
      check("setup-down-focus", Near(settings, {245,138,53}, 4) && !Near(play, {245,138,53}, 40),
            "{\"play\":" + Json(play) + ",\"settings\":" + Json(settings) + "}");
    }
    key(rex::ui::VirtualKey::kDown);
    key(rex::ui::VirtualKey::kDown);
    key(rex::ui::VirtualKey::kReturn);
    check("setup-quit", quits == 1 && played == 0, "{\"quits\":" + std::to_string(quits) + "}");
    // The mouse over DLC focuses it; a click runs it.
    rex::ui::MouseEvent move(nullptr, rex::ui::MouseEvent::Button::kNone, 500, 455);
    rex::ui::MouseEvent press(nullptr, rex::ui::MouseEvent::Button::kLeft, 500, 455);
    menu.OnMouseMove(move);
    menu.OnMouseDown(press);
    menu.OnMouseUp(press);
    check("setup-click", dlcs == 1 && move.is_handled(), "{\"dlcs\":" + std::to_string(dlcs) + "}");
    std::vector<uint8_t> hovered;
    if (frame("setup-hover", hovered)) {
      const auto dlc = at(hovered, 500, 455);
      check("setup-hover-focus", Near(dlc, {245,138,53}, 4), Json(dlc));
    }

    check("setup-no-embedded-game-art", narutorise::menu::Asset("images/cover.png").empty() &&
          narutorise::menu::Asset("images/spiral.png").empty() &&
          narutorise::menu::Asset("local/game-logo").empty(), "{}");
    if (const char* root = std::getenv("NARUTORISE_TEST_GAME_ROOT")) {
      const auto colored_logo = [&](const std::vector<uint8_t>& pixels) {
        unsigned count = 0;
        for (uint32_t y=100;y<280;++y) for(uint32_t x=470;x<810;++x) {
          const auto p=at(pixels,x,y); count += p[0]>120 && p[0]>p[1]*1.4 && p[1]>20;
        }
        return count;
      };
      state.game_root = root; state.game.disc_image = false;
      menu.RefreshSetup(state);
      std::vector<uint8_t> logo;
      unsigned colors = 0;
      for (int i=0;i<100 && colors<500;++i) {
        frame("setup-logo-await",logo); colors=colored_logo(logo); Sleep(5);
      }
      check("setup-local-logo-rendered",colors>500,"{\"colored_pixels\":"+std::to_string(colors)+"}");
      // Transparent corners must preserve the procedural background.
      check("setup-logo-alpha",Near(at(logo,465,97),at(shown,465,97),4),Json(at(logo,465,97)));
      std::ofstream(std::string(argv[1])+".logo-1280x720.bgra",std::ios::binary)
          .write(reinterpret_cast<const char*>(logo.data()),std::streamsize(logo.size()));
      state.game_root.clear(); menu.RefreshSetup(state);
      frame("setup-logo-cleared",logo);
      check("setup-logo-root-change-fallback",colored_logo(logo)==0,"{}");
      state.game_root = root; menu.RefreshSetup(state);
      frame("setup-logo-pending",logo);
      menu.HideSetup(); state.game_root.clear(); menu.ShowSetup(state,actions);
      for(int i=0;i<10;++i) { Sleep(5); frame("setup-logo-stale",logo); }
      check("setup-logo-late-result-discarded",colored_logo(logo)==0,"{}");
      // Return focus to DLC for the remaining modal/handover regressions.
      menu.OnMouseMove(move); frame("setup-logo-restore-focus",logo);
    }

    // The UI screen before the game: the engine goes, the screen's own engine
    int confirmed = 0, canceled = 0;
    menu.ShowDialog("Unsupported version", "Choose another <game> & keep the current path",
                    {{"Choose another", [&] { ++confirmed; }}, {"Cancel", [&] { ++canceled; }}});
    std::vector<uint8_t> modal;
    frame("dialog", modal);
    key(rex::ui::VirtualKey::kReturn);
    frame("dialog-cancel", modal);
    check("dialog-cancel-default", canceled == 1 && confirmed == 0, "{}");
    menu.ShowDialog("Checking the game", "Please wait");
    key(rex::ui::VirtualKey::kReturn);
    frame("dialog-busy", modal);
    check("dialog-busy-blocks-setup", played == 0 && quits == 1, "{}");
    menu.HideDialog();

    int changed = -1, tab = -1, closed = 0;
    narutorise::menu::PanelRow option{"setting", "Option", "Off", "A reusable setting", false, true,
                                     {"Off", "On"}, 0, [&](int value) { changed = value; }, {}};
    narutorise::menu::PanelRow long_device{"long-device", "Controller", std::string(100, 'W'), "A long device name"};
    menu.ShowPanel("Settings", {"Game", "Display"}, 0, {option, long_device}, [&](int value) { tab = value; },
                     [&] { ++closed; menu.HidePanel(); });
    frame("panel", modal);
    check("panel-v4-focus-color", Near(at(modal, 500, 170), {35,44,72}, 4), Json(at(modal, 500,170)));
    check("panel-v4-focus-edge", Near(at(modal, 140, 150), orange, 4), Json(at(modal, 140,150)));
    uint32_t escaped = 0;
    for (uint32_t y = 185; y < 215; ++y) for (uint32_t x = 678; x < 691; ++x) {
      const auto p = at(modal, x, y); escaped += p[0] + p[1] + p[2] > 300;
    }
    check("long-values-clipped-before-divider", escaped == 0, "{\"pixels\":" + std::to_string(escaped) + "}");
    std::ofstream(std::string(argv[1]) + ".panel-1280x720.bgra", std::ios::binary)
        .write(reinterpret_cast<const char*>(modal.data()), std::streamsize(modal.size()));
    key(rex::ui::VirtualKey::kRight);
    frame("panel-change", modal);
    check("panel-arrow-changes-value", changed == 1, "{}");
    changed = -1;
    rex::ui::MouseEvent arrow_move(nullptr, rex::ui::MouseEvent::Button::kNone, 655, 156);
    rex::ui::MouseEvent arrow_click(nullptr, rex::ui::MouseEvent::Button::kLeft, 655, 156);
    menu.OnMouseMove(arrow_move); menu.OnMouseDown(arrow_click); menu.OnMouseUp(arrow_click);
    frame("panel-mouse-arrow", modal);
    check("panel-mouse-arrow-adjusts", changed == 1, "{}");
    key(rex::ui::VirtualKey::kNext);
    frame("panel-tab", modal);
    check("panel-next-tab", tab == 1, "{}");
    key(rex::ui::VirtualKey::kEscape);
    frame("panel-back", modal);
    check("panel-back-closes", closed == 1 && !menu.PanelShown(), "{}");

    narutorise::menu::TextSystem translations;
    for (const auto& language : Localization::GetLanguages()) {
      Localization::SetLanguage(language.language);
      std::string translated;
      check("language-" + std::string(language.code), translations.TranslateString(translated, "@choose") == 1 &&
            !translated.empty() && translated != "@choose" && !std::string(narutorise::menu::Text("cancel")).empty(), "{}");
    }
    Localization::SetLanguage(Localization::Language::English);
    narutorise::menu::PadInput navigation;
    std::vector<rex::ui::VirtualKey> navigation_keys;
    const auto emit = [&](rex::ui::VirtualKey value) { navigation_keys.push_back(value); };
    const auto now = std::chrono::steady_clock::now();
    navigation.Navigate(2, now, emit);
    navigation.Navigate(2, now + std::chrono::milliseconds(100), emit);
    navigation.Navigate(2, now + std::chrono::milliseconds(400), emit);
    navigation.Navigate(0, now + std::chrono::milliseconds(450), emit);
    navigation.Navigate(16, now + std::chrono::milliseconds(500), emit);
    navigation.Navigate(16, now + std::chrono::milliseconds(1000), emit);
    check("pad-repeat-and-confirm-edge", navigation_keys == std::vector<rex::ui::VirtualKey>{
        rex::ui::VirtualKey::kDown, rex::ui::VirtualKey::kDown, rex::ui::VirtualKey::kReturn}, "{}");
    {
      narutorise::menu::PadInput shortcut_navigation;
      narutorise::controls::Inventory inputs; inputs.devices.emplace_back();
      auto& input = inputs.devices.front().raw;
      int shortcuts = 0;
      shortcut_navigation.SetShortcut("Back+Start", [&] { ++shortcuts; });
      input.buttons = 0x10; shortcut_navigation.PollDevices(inputs, false, emit);
      input.buttons = 0; shortcut_navigation.PollDevices(inputs, false, emit);
      input.buttons = 0x20; shortcut_navigation.PollDevices(inputs, false, emit);
      check("pad-shortcut-needs-both-buttons", shortcuts == 0, "{}");
      input.buttons = 0x30; shortcut_navigation.PollDevices(inputs, false, emit);
      check("pad-shortcut-opens-on-press", shortcuts == 1, "{}");
      shortcut_navigation.PollDevices(inputs, true, emit);
      shortcut_navigation.PollDevices(inputs, true, emit);
      check("pad-shortcut-held-does-not-repeat", shortcuts == 1, "{}");
      input.buttons = 0x10; shortcut_navigation.PollDevices(inputs, true, emit);
      input.buttons = 0x30; shortcut_navigation.PollDevices(inputs, true, emit);
      check("pad-shortcut-release-allows-next-press", shortcuts == 2, "{}");
    }
    {
      narutorise::menu::PadInput preview_navigation;
      narutorise::controls::Inventory inputs;inputs.devices.emplace_back();
      auto& input=inputs.devices.front().raw;input.axes={.9,-.9,.8,-.8,.7,.6};
      navigation_keys.clear();preview_navigation.PollDevices(inputs,true,emit,false);
      check("pad-preview-axes-do-not-navigate",navigation_keys.empty(),"{}");
      input.buttons=1;preview_navigation.PollDevices(inputs,true,emit,false);
      check("pad-preview-dpad-navigates",navigation_keys==std::vector{rex::ui::VirtualKey::kUp},"{}");
      input.buttons=0;navigation_keys.clear();preview_navigation.PollDevices(inputs,true,emit);
      check("pad-preview-exit-held-stick-suppressed",navigation_keys.empty(),"{}");
      input.axes={};preview_navigation.PollDevices(inputs,true,emit);
      input.axes[0]=.9;preview_navigation.PollDevices(inputs,true,emit);
      check("pad-preview-neutral-restores-stick-navigation",navigation_keys==std::vector{rex::ui::VirtualKey::kRight},"{}");
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_VirtualJoystickDesc virtual_pad;
    SDL_INIT_INTERFACE(&virtual_pad);
    virtual_pad.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    virtual_pad.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    virtual_pad.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    virtual_pad.name = "Menu navigation test";
    const auto pad_id = SDL_AttachVirtualJoystick(&virtual_pad);
    auto* joystick = pad_id ? SDL_OpenJoystick(pad_id) : nullptr;
    check("pad-virtual-attached", joystick != nullptr, "{}");
    if (joystick) {
      navigation.Initialize(); navigation_keys.clear();
      SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
      navigation.Poll(true, emit);
      check("pad-sdl-navigation", std::find(navigation_keys.begin(), navigation_keys.end(), rex::ui::VirtualKey::kDown)
            != navigation_keys.end(), "{}");
      int shortcuts = 0;
      navigation.SetShortcut("L3+R3", [&] { ++shortcuts; });
      SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
      SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, true);
      SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
      navigation.Poll(false, emit);
      check("pad-remapped-shortcut-on-press", shortcuts == 1, "{}");
      navigation.Poll(false, emit); navigation.Poll(false, emit);
      check("pad-remapped-held-shortcut-once", shortcuts == 1, "{}");
      SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
      navigation.Poll(false, emit);
      SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
      navigation.Poll(false, emit);
      check("pad-remapped-shortcut-next-press", shortcuts == 2, "{}");
      navigation.Shutdown(); SDL_CloseJoystick(joystick); SDL_DetachVirtualJoystick(pad_id);
    }

    const auto settings_dir = std::filesystem::path(argv[1]).parent_path() / "settings";
    std::filesystem::create_directories(settings_dir);
    const auto settings_path = settings_dir / "narutorise.toml";
    std::ofstream(settings_path) << "unknown_option = 42\nuser_language = 1\nfullscreen = false\nshow_fps_overlay = false\nmnk_mode = true\nmnk_mouse = true\n";
    narutorise::menu::Settings settings_model;
    settings_model.Initialize(settings_dir);
    settings_model.Open(menu, 0, false, {}, [](const std::string&) {}, [] {});
    frame("settings-game", modal);
    key(rex::ui::VirtualKey::kDown); key(rex::ui::VirtualKey::kRight);
    frame("settings-language", modal);
    auto saved = toml::parse_file(settings_path.string());
    check("settings-save-language", saved["user_language"].value<int>() == 4 && saved["unknown_option"].value<int>() == 42, "{}");
    key(rex::ui::VirtualKey::kNext); frame("settings-display", modal);
    key(rex::ui::VirtualKey::kDown); key(rex::ui::VirtualKey::kDown); key(rex::ui::VirtualKey::kRight);
    frame("settings-resolution", modal);
    saved = toml::parse_file(settings_path.string());
    check("settings-fractional-resolution", saved["internal_resolution_scale"].value<double>() == 1.5 &&
          saved["resolution_scale"].value<int>() == 1, "{}");
    key(rex::ui::VirtualKey::kDown); key(rex::ui::VirtualKey::kReturn); frame("settings-number-description", modal);
    int numeric_choices = 0;
    for (int i = 0; i < Rml::GetContext("game")->GetNumDocuments(); ++i) {
      auto* document = Rml::GetContext("game")->GetDocument(i);
      if (auto* detail = document->IsVisible() ? document->GetElementById("detail") : nullptr) {
        Rml::ElementList choices; detail->GetElementsByClassName(choices, "choice"); numeric_choices += int(choices.size());
      }
    }
    check("numeric-value-keeps-description-panel", numeric_choices == 0, "{}");
    key(rex::ui::VirtualKey::kRight); frame("settings-number-adjusted", modal);
    saved = toml::parse_file(settings_path.string());
    check("numeric-arrow-adjustment-saved", std::abs(saved["brightness"].value_or(0.0) - .51) < .0001, "{}");
    for (int i = 0; i < 3; ++i) { key(rex::ui::VirtualKey::kNext); frame("settings-next-" + std::to_string(i), modal); }
    key(rex::ui::VirtualKey::kDown); key(rex::ui::VirtualKey::kDown); key(rex::ui::VirtualKey::kRight);
    frame("settings-fps", modal);
    saved = toml::parse_file(settings_path.string());
    check("settings-fps-saved", saved["show_fps_overlay"].value<bool>() == true, "{}");
    key(rex::ui::VirtualKey::kPrior); frame("settings-controls", modal);
    const auto geometry = [&](const std::string& name) {
      auto* context = Rml::GetContext("game"); Rml::Element* options = nullptr;
      for (int i = 0; context && i < context->GetNumDocuments(); ++i) {
        auto* document = context->GetDocument(i);
        if (document->IsVisible() && document->GetElementById("options")) options = document->GetElementById("options");
      }
      int rows = 0; bool valid = options != nullptr;
      if (options) for (int i = 0; i < options->GetNumChildren(); ++i) {
        auto* row = options->GetChild(i); if (!row->HasAttribute("row")) continue;
        ++rows;
        const auto size = row->GetBox().GetSize(Rml::BoxArea::Border);
        auto* label = row->GetChild(0); auto* value = row->GetChild(1);
        const auto label_right = label->GetAbsoluteOffset().x + label->GetBox().GetSize().x;
        valid &= size.x >= 530 && size.x <= 540 && size.y >= 39 && size.y <= 41;
        valid &= label_right <= value->GetAbsoluteOffset().x + 1;
      }
      check(name, valid && rows > 0, "{\"rows\":" + std::to_string(rows) + "}");
    };
    geometry("controls-rows-have-full-width");
    uint32_t overflow_text = 0;
    for (uint32_t y = 218; y < 246; ++y) for (uint32_t x = 678; x < 691; ++x) {
      const auto p = at(modal, x, y); overflow_text += p[0] + p[1] + p[2] > 300;
    }
    check("controls-text-clipped-before-divider", overflow_text == 0,
          "{\"overflow_pixels\":" + std::to_string(overflow_text) + "}");
    std::ofstream(std::string(argv[1]) + ".controls-1280x720.bgra", std::ios::binary)
        .write(reinterpret_cast<const char*>(modal.data()), std::streamsize(modal.size()));
    const auto visible_document = [&](const char* id) -> Rml::ElementDocument* {
      auto* context = Rml::GetContext("game");
      for (int i=0;i<context->GetNumDocuments();++i) {
        auto* doc=context->GetDocument(i); if(doc->IsVisible()&&doc->GetElementById(id))return doc;
      }
      return nullptr;
    };
    const auto focus_id = [&](const char* id) {
      auto* context=Rml::GetContext("game");
      for(int i=0;i<context->GetNumDocuments();++i){auto* doc=context->GetDocument(i);if(doc->IsVisible())if(auto* e=doc->GetElementById(id)){e->Focus();return true;}}
      return false;
    };
    {
      menu.HidePanel();auto nav_config=narutorise::controls::Config{};nav_config.version=1;
      narutorise::controls::Assign(nav_config,0,"keyboard","Keyboard and mouse");int nav_saves=0;bool nav_closed=false;
      menu.ShowControls(nav_config,[&](const auto& next){nav_config=next;++nav_saves;return true;},[&]{nav_closed=true;menu.HidePanel();},"F2");frame("focus-route-open",modal);
      const auto focused_id=[&]{auto* e=Rml::GetContext("game")->GetFocusElement();return e?e->GetId():std::string{};};
      check("focus-route-initial-category",focused_id()=="device-category","{}");
      key(rex::ui::VirtualKey::kReturn);frame("focus-route-enter-device",modal);
      check("focus-route-category-enters-content",focused_id()=="device-0"&&nav_saves==0,"{}");
      if(focused_id()=="device-0"){
        key(rex::ui::VirtualKey::kEscape);frame("focus-route-back-sidebar",modal);
        check("focus-route-back-retains-window",!nav_closed&&focused_id()=="device-category","{}");
        key(rex::ui::VirtualKey::kDown);frame("focus-route-buttons-category",modal);
        check("focus-route-down-selects-category",focused_id()=="category-1","{}");
        key(rex::ui::VirtualKey::kReturn);frame("focus-route-enter-buttons",modal);
        check("focus-route-buttons-first-field",focused_id()=="bind-0-0"&&nav_saves==0,"{}");
        key(rex::ui::VirtualKey::kRight);frame("focus-route-secondary-field",modal);
        check("focus-route-alternatives-accessible",focused_id()=="bind-0-1","{}");
        key(rex::ui::VirtualKey::kReturn);frame("focus-route-inline-capture",modal);
        key(rex::ui::VirtualKey::kF5);frame("focus-route-binding-changed",modal);
        const auto alternatives=narutorise::controls::Alternatives(nav_config.ports[0].keys[0]);
        check("focus-route-edits-without-mouse",nav_saves==1&&alternatives.size()>1&&alternatives[1]=="F5","{}");
        key(rex::ui::VirtualKey::kEscape);frame("focus-route-buttons-back",modal);
        check("focus-route-return-selected-category",!nav_closed&&focused_id()=="category-1","{}");
        key(rex::ui::VirtualKey::kDown);key(rex::ui::VirtualKey::kDown);frame("focus-route-movement-category",modal);
        key(rex::ui::VirtualKey::kReturn);frame("focus-route-enter-movement",modal);
        check("focus-route-movement-first-field",focused_id()=="bind-12-0","{}");
        key(rex::ui::VirtualKey::kEscape);frame("focus-route-exit-content",modal);
        key(rex::ui::VirtualKey::kUp);key(rex::ui::VirtualKey::kRight);frame("focus-route-enter-triggers",modal);
        check("focus-route-right-enters-triggers",focused_id()=="bind-6-0","{}");
        key(rex::ui::VirtualKey::kDown);frame("focus-route-second-trigger",modal);
        check("focus-route-both-triggers-accessible",focused_id()=="bind-7-0","{}");
        key(rex::ui::VirtualKey::kEscape);frame("focus-route-triggers-back",modal);
        key(rex::ui::VirtualKey::kUp);key(rex::ui::VirtualKey::kReturn);frame("focus-route-buttons-again",modal);
        for(int i=0;i<8;++i)key(rex::ui::VirtualKey::kDown);
        frame("focus-route-direction-link",modal);
        check("focus-route-direction-link-accessible",focused_id()=="mapping-more","{}");
        key(rex::ui::VirtualKey::kReturn);frame("focus-route-enter-directions",modal);
        check("focus-route-directions-fields",focused_id()=="bind-20-0","{}");
        key(rex::ui::VirtualKey::kEscape);frame("focus-route-directions-back",modal);
        check("focus-route-directions-return-to-sidebar",focused_id()=="category-1","{}");
        key(rex::ui::VirtualKey::kEscape);frame("focus-route-exit-window",modal);
        check("focus-route-close-from-sidebar",nav_closed,"{}");
      }
      menu.HidePanel();settings_model.Open(menu,3,false,{},[](const std::string&){},[]{});frame("focus-route-restore-settings",modal);
    }
    {
      for (bool standalone_first : {false, true}) {
        narutorise::controls::Config bindings;
        bindings.version = 1;
        narutorise::controls::Assign(bindings, 0, "keyboard", "Keyboard and mouse");
        bindings.ports[0].keys = {};
        bindings.ports[0].keys[0] = standalone_first ? "Shift" : "Shift+G";
        int binding_saves = 0;
        menu.ShowControls(bindings, [&](const auto& next) { bindings = next; ++binding_saves; return true; }, {}, "F2");
        frame("modifier-conflict-open", modal);
        focus_id("category-1"); key(rex::ui::VirtualKey::kReturn); frame("modifier-conflict-buttons", modal);
        focus_id("bind-1-0"); key(rex::ui::VirtualKey::kReturn); frame("modifier-conflict-armed", modal);
        rex::ui::KeyEvent shift(nullptr, rex::ui::VirtualKey::kShift, 1, false, false, false, false, false);
        rex::ui::KeyEvent g(nullptr, rex::ui::VirtualKey::kG, 1, false, false, false, false, false);
        menu.OnKeyDown(shift);
        if (standalone_first) { menu.OnKeyDown(g); menu.OnKeyUp(g); }
        menu.OnKeyUp(shift);
        frame("modifier-conflict-rejected", modal);
        auto* doc = visible_document("controls-window");
        check(standalone_first ? "new-chord-preserves-standalone" : "new-standalone-preserves-chord",
              binding_saves == 0 && bindings.ports[0].keys[1].empty() && doc &&
              doc->GetElementById("bind-1-0")->IsClassSet("editing") &&
              doc->GetElementById("controls-status")->GetInnerRML().find(narutorise::menu::Text("c-modifier-conflict")) != std::string::npos, "{}");
        key(rex::ui::VirtualKey::kEscape); frame("modifier-conflict-cancel", modal);
        if (!standalone_first) {
          focus_id("bind-0-0"); key(rex::ui::VirtualKey::kReturn); frame("modifier-replace-own-binding", modal);
          key(rex::ui::VirtualKey::kShift); frame("modifier-replace-own-binding-saved", modal);
          check("replacing-own-chord-with-modifier-is-allowed",
                binding_saves == 1 && bindings.ports[0].keys[0] == "Shift", "{}");
        }
        menu.HidePanel();
      }
      settings_model.Open(menu, 3, false, {}, [](const std::string&) {}, [] {});
      frame("modifier-conflict-restore-settings", modal);
    }
    focus_id("configure-controls");key(rex::ui::VirtualKey::kReturn);frame("controls-dedicated",modal);
    auto* controls_doc=visible_document("controls-window");
    check("controls-dedicated-window",controls_doc!=nullptr,"{}");
    if(controls_doc){
      check("controls-four-player-tabs",controls_doc->GetElementById("players")->GetNumChildren()==4,"{}");
      focus_id("category-1");key(rex::ui::VirtualKey::kReturn);frame("controls-keyboard-buttons",modal);
      check("controls-keyboard-bank",controls_doc->GetElementById("bind-0-0")!=nullptr&&controls_doc->GetElementById("category-1")->IsClassSet("selected"),"{}");
      check("controls-long-key-name-has-space",controls_doc->GetElementById("bind-1-1")->GetBox().GetSize(Rml::BoxArea::Border).x>=94,"{}");
      std::ofstream(std::string(argv[1])+".controls-dedicated-1280x720.bgra",std::ios::binary).write(reinterpret_cast<const char*>(modal.data()),std::streamsize(modal.size()));
      focus_id("bind-0-0");key(rex::ui::VirtualKey::kReturn);frame("controls-inline-wait",modal);
      std::ofstream(std::string(argv[1])+".capture-inline-1280x720.bgra",std::ios::binary).write(reinterpret_cast<const char*>(modal.data()),std::streamsize(modal.size()));
      check("capture-stays-in-own-line",controls_doc->GetElementById("bind-0-0")->IsClassSet("editing")&&!visible_document("dialog-title"),"{}");
      key(rex::ui::VirtualKey::kNext);frame("capture-block-player",modal);
      check("capture-blocks-player-navigation",controls_doc->GetElementById("player-0")->IsClassSet("selected"),"{}");
      // Physical PageDown is itself a valid keyboard binding; it must bind
      // without switching player, then a new capture may replace it with H.
      focus_id("bind-0-0");key(rex::ui::VirtualKey::kReturn);frame("capture-key-rearm",modal);
      key(rex::ui::VirtualKey::kH);frame("capture-key-saved",modal);
      saved=toml::parse_file(settings_path.string());
      check("capture-saves-own-port-bank",saved["controls"]["player1"]["keys"]["keybind_a"].value<std::string>()=="H"&&saved["keybind_a"].value<std::string>()=="Space","{}");
      focus_id("bind-0-0");key(rex::ui::VirtualKey::kReturn);frame("capture-cancel-start",modal);
      key(rex::ui::VirtualKey::kEscape);frame("capture-canceled",modal);
      saved=toml::parse_file(settings_path.string());
      check("capture-Escape-preserves-binding",saved["controls"]["player1"]["keys"]["keybind_a"].value<std::string>()=="H","{}");
      focus_id("bind-0-0");key(rex::ui::VirtualKey::kReturn);frame("capture-delete-start",modal);
      key(rex::ui::VirtualKey::kDelete);frame("capture-deleted",modal);
      saved=toml::parse_file(settings_path.string());
      check("capture-Delete-removes-binding",saved["controls"]["player1"]["keys"]["keybind_a"].value<std::string>()=="","{}");
      focus_id("bind-0-0");key(rex::ui::VirtualKey::kReturn);frame("capture-reserved-start",modal);
      key(rex::ui::VirtualKey::kF1);frame("capture-reserved-key",modal);
      check("capture-reserved-stays-in-line",controls_doc->GetElementById("bind-0-0")->IsClassSet("editing"),"{}");
      key(rex::ui::VirtualKey::kEscape);frame("capture-reserved-cancel",modal);
      focus_id("bind-0-0");key(rex::ui::VirtualKey::kReturn);frame("capture-mouse-start",modal);
      rex::ui::MouseEvent mouse_binding(nullptr,rex::ui::MouseEvent::Button::kLeft,900,250);
      menu.OnMouseDown(mouse_binding);menu.OnMouseUp(mouse_binding);frame("capture-mouse-saved",modal);
      saved=toml::parse_file(settings_path.string());
      check("capture-mouse-conflict-local",saved["controls"]["player1"]["keys"]["keybind_a"].value<std::string>()=="LMB"&&saved["controls"]["player1"]["keys"]["keybind_x"].value<std::string>()==",J"&&saved["controls"]["player2"]["keys"]["keybind_x"].value<std::string>()=="LMB,J","{}");
      focus_id("mapping-more");key(rex::ui::VirtualKey::kReturn);frame("controls-keyboard-directions",modal);
      check("controls-directions-own-bank",controls_doc->GetElementById("bind-20-0")!=nullptr,"{}");
      focus_id("mapping-more");key(rex::ui::VirtualKey::kReturn);frame("controls-buttons-return",modal);
      focus_id("category-4");key(rex::ui::VirtualKey::kReturn);frame("controls-movement-camera",modal);
      check("controls-keyboard-movement-camera",controls_doc->GetElementById("bind-16-0")&&controls_doc->GetElementById("plus-sensitivity"),"{}");
      {auto* first=controls_doc->GetElementById("bind-16-0");auto* second=controls_doc->GetElementById("bind-16-1");
        check("camera-binding-geometry",second->GetBox().GetSize().x>=54,fmt::format("{{\"row\":{},\"container\":{},\"first\":{},\"second\":{},\"style\":\"{}\"}}",first->GetParentNode()->GetParentNode()->GetBox().GetSize(Rml::BoxArea::Border).x,first->GetParentNode()->GetBox().GetSize(Rml::BoxArea::Border).x,first->GetBox().GetSize(Rml::BoxArea::Border).x,second->GetBox().GetSize(Rml::BoxArea::Border).x,second->GetProperty("width")->ToString()));}
      std::ofstream(std::string(argv[1])+".movement-camera-1280x720.bgra",std::ios::binary).write(reinterpret_cast<const char*>(modal.data()),std::streamsize(modal.size()));
      rex::ui::MouseEvent camera_baseline(nullptr,rex::ui::MouseEvent::Button::kNone,900,300);menu.OnMouseMove(camera_baseline);
      focus_id("bind-19-1");key(rex::ui::VirtualKey::kReturn);frame("keyboard-camera-inline-capture",modal);
      rex::ui::MouseEvent camera_motion(nullptr,rex::ui::MouseEvent::Button::kNone,880,300,0,0,-20,0);menu.OnMouseMove(camera_motion);frame("keyboard-camera-motion-bound",modal);
      saved=toml::parse_file(settings_path.string());
      check("keyboard-camera-motion-own-bank",saved["controls"]["player1"]["keys"]["keybind_rstick_right"].value<std::string>()=="Right,MouseLeft"&&saved["controls"]["player1"]["keys"]["keybind_rstick_left"].value<std::string>()=="Left"&&controls_doc->GetElementById("bind-16-0")&&Rml::GetContext("game")->GetFocusElement()->GetId()=="bind-19-1","{}");
      check("controls-keyboard-rumble-disabled",controls_doc->GetElementById("category-5")->HasAttribute("disabled"),"{}");
      key(rex::ui::VirtualKey::kNext);frame("controls-player-two",modal);
      check("controls-next-player",controls_doc->GetElementById("player-1")->IsClassSet("selected"),"{}");
      const auto settings_bytes=[&]{std::ifstream in(settings_path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
      const auto keyboard_button="device-"+std::to_string(narutorise::controls::Hardware().Devices()->devices.size()-1);
      const auto before_transfer=settings_bytes();
      focus_id(keyboard_button.c_str());key(rex::ui::VirtualKey::kReturn);frame("device-transfer-confirmation",modal);
      check("device-transfer-confirmation-before-change",controls_doc->GetElementById("transfer-confirm")&&narutorise::controls::Current()->ports[0].device=="keyboard"&&narutorise::controls::Current()->ports[1].device.empty(),"{}");
      std::ofstream(std::string(argv[1])+".transfer-1280x720.bgra",std::ios::binary).write(reinterpret_cast<const char*>(modal.data()),std::streamsize(modal.size()));
      focus_id("transfer-cancel");key(rex::ui::VirtualKey::kReturn);frame("device-transfer-canceled",modal);
      check("device-transfer-cancel-does-not-save",settings_bytes()==before_transfer,"{}");
      focus_id(keyboard_button.c_str());key(rex::ui::VirtualKey::kReturn);frame("device-transfer-reopen",modal);
      focus_id("transfer-confirm");key(rex::ui::VirtualKey::kReturn);frame("device-transfer-committed",modal);
      saved=toml::parse_file(settings_path.string());
      check("device-transfer-preserves-both-profiles",saved["controls"]["player1"]["device"].value<std::string>()==""&&saved["controls"]["player2"]["device"].value<std::string>()=="keyboard"&&saved["controls"]["player1"]["keys"]["keybind_a"].value<std::string>()=="LMB"&&saved["controls"]["player2"]["keys"]["keybind_a"].value<std::string>()=="Space","{}");
      // Transfer the same real keyboard through J3/J4, without needing four pads.
      for(int player=2;player<4;++player){
        key(rex::ui::VirtualKey::kNext);frame("device-transfer-port-"+std::to_string(player),modal);
        focus_id(keyboard_button.c_str());key(rex::ui::VirtualKey::kReturn);frame("device-transfer-prompt-"+std::to_string(player),modal);
        focus_id("transfer-confirm");key(rex::ui::VirtualKey::kReturn);frame("device-transfer-apply-"+std::to_string(player),modal);
        check("device-transfer-isolated-port-"+std::to_string(player),narutorise::controls::Current()->ports[player].device=="keyboard"&&narutorise::controls::Current()->ports[player-1].device.empty(),"{}");
      }
      key(rex::ui::VirtualKey::kNext);frame("device-return-player-one",modal);
      focus_id(keyboard_button.c_str());key(rex::ui::VirtualKey::kReturn);frame("device-return-keyboard-prompt",modal);
      focus_id("transfer-confirm");key(rex::ui::VirtualKey::kReturn);frame("device-return-keyboard",modal);
      focus_id("device-none");key(rex::ui::VirtualKey::kReturn);frame("device-none-selected",modal);
      check("device-none-effective",narutorise::controls::Current()->ports[0].device.empty()&&!rex::input::mnk::GetProfile()->enabled&&controls_doc->GetElementById("category-1")->HasAttribute("disabled"),"{}");
      focus_id(keyboard_button.c_str());key(rex::ui::VirtualKey::kReturn);frame("device-keyboard-available-selected",modal);
      check("device-available-no-transfer",!controls_doc->GetElementById("transfer-confirm")&&narutorise::controls::Current()->ports[0].device=="keyboard","{}");
      key(rex::ui::VirtualKey::kNext);frame("controls-return-player-two",modal);
      key(rex::ui::VirtualKey::kPrior);frame("controls-player-one",modal);
      check("controls-previous-player",controls_doc->GetElementById("player-0")->IsClassSet("selected"),"{}");
    }
    menu.HidePanel();
    // An SDL virtual device is a test fixture only; it exercises the exact
    // polling path used by hardware without installing a fictitious device.
    SDL_VirtualJoystickDesc capture_pad; SDL_INIT_INTERFACE(&capture_pad);
    capture_pad.type=SDL_JOYSTICK_TYPE_GAMEPAD;capture_pad.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;capture_pad.naxes=SDL_GAMEPAD_AXIS_COUNT;
    capture_pad.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;capture_pad.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;capture_pad.name="Controls capture fixture";
    struct RumbleProbe { Uint16 low=0,high=0;int calls=0;bool fail=false; } rumble_probe;
    capture_pad.userdata=&rumble_probe;
    capture_pad.Rumble=+[](void* data,Uint16 low,Uint16 high)->bool {auto& p=*static_cast<RumbleProbe*>(data);p.low=low;p.high=high;++p.calls;return !p.fail;};
    auto capture_id=SDL_AttachVirtualJoystick(&capture_pad);auto* capture_joystick=capture_id?SDL_OpenJoystick(capture_id):nullptr;
    check("capture-virtual-device-attached",capture_joystick!=nullptr,"{}");
    if(capture_joystick){
      narutorise::controls::Hardware().Poll("sdl",true);
      auto inventory=narutorise::controls::Hardware().Devices();
      const narutorise::controls::Device* device=nullptr;
      for(const auto& d:inventory->devices)if(d.pad&&SDL_GetGamepadID(d.pad)==capture_id)device=&d;
      check("capture-virtual-device-real-inventory",device!=nullptr,"{}");
      if(device){
        auto virtual_config=settings_model.config().controls;
        narutorise::controls::Assign(virtual_config,0,device->key,device->name);
        const auto focused_id=[&]{auto* e=Rml::GetContext("game")->GetFocusElement();return e?e->GetId():std::string{};};
        const auto pad_key=[&](SDL_GamepadButton button,const std::string& label){
          SDL_SetJoystickVirtualButton(capture_joystick,button,true);frame(label+"-down",modal);
          SDL_SetJoystickVirtualButton(capture_joystick,button,false);frame(label+"-up",modal);
        };
        menu.ShowControls(virtual_config,[&](const auto& next){virtual_config=next;narutorise::controls::Publish(next);return true;},[]{},"F2");
        frame("capture-pad-window",modal);
        pad_key(SDL_GAMEPAD_BUTTON_DPAD_DOWN,"pad-focus-buttons-category");
        check("pad-focus-category-without-mouse",focused_id()=="category-1","{}");
        pad_key(SDL_GAMEPAD_BUTTON_SOUTH,"pad-focus-enter-buttons");
        auto* pad_doc=visible_document("controls-window");
        check("pad-focus-content-without-capturing",focused_id()=="bind-0-0"&&!pad_doc->GetElementById("bind-0-0")->IsClassSet("editing"),"{}");
        SDL_SetJoystickVirtualButton(capture_joystick,SDL_GAMEPAD_BUTTON_SOUTH,true);
        frame("capture-initiator-press",modal);frame("capture-initiator-held",modal);
        check("capture-does-not-bind-initiator",virtual_config.ports[0].pad[0]==narutorise::controls::DefaultPad()[0]&&pad_doc->GetElementById("bind-0-0")->IsClassSet("editing"),"{}");
        SDL_SetJoystickVirtualButton(capture_joystick,SDL_GAMEPAD_BUTTON_SOUTH,false);frame("capture-initiator-release",modal);
        const std::pair<SDL_GamepadButton,int> buttons[]={{SDL_GAMEPAD_BUTTON_EAST,0x2000},{SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,0x100},{SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,0x200},{SDL_GAMEPAD_BUTTON_DPAD_UP,1},{SDL_GAMEPAD_BUTTON_DPAD_DOWN,2},{SDL_GAMEPAD_BUTTON_DPAD_LEFT,4},{SDL_GAMEPAD_BUTTON_DPAD_RIGHT,8},{SDL_GAMEPAD_BUTTON_LEFT_STICK,0x40},{SDL_GAMEPAD_BUTTON_RIGHT_STICK,0x80}};
        bool first_binding=true;
        for(auto [button,bit]:buttons){
          if(!first_binding){focus_id("bind-0-0");key(rex::ui::VirtualKey::kReturn);frame("capture-pad-arm-"+std::to_string(bit),modal);}first_binding=false;
          SDL_SetJoystickVirtualButton(capture_joystick,button,true);frame("capture-pad-source-"+std::to_string(bit),modal);
          frame("capture-pad-held-"+std::to_string(bit),modal);
          check("capture-pad-bound-no-menu-action-"+std::to_string(bit),virtual_config.ports[0].pad[0].code==bit&&pad_doc->GetElementById("player-0")->IsClassSet("selected")&&menu.PanelShown(),"{}");
          SDL_SetJoystickVirtualButton(capture_joystick,button,false);frame("capture-pad-release-"+std::to_string(bit),modal);
        }
        for(int sample=0;sample<10;++sample){
          const int axis=sample<8?sample/2:sample-4;const int input_value=axis<4&&(sample%2)?-28000:28000;
          focus_id(axis<4?"category-4":"category-3");key(rex::ui::VirtualKey::kReturn);frame("capture-axis-page-"+std::to_string(axis),modal);
          focus_id(axis<4?"bind-12-0":"bind-6-0");key(rex::ui::VirtualKey::kReturn);frame("capture-axis-arm-"+std::to_string(axis),modal);
          SDL_SetJoystickVirtualAxis(capture_joystick,axis,input_value);frame("capture-axis-value-"+std::to_string(axis),modal);
          check("capture-signed-axis-"+std::to_string(axis),virtual_config.ports[0].pad[axis<4?12:6].kind==narutorise::controls::SourceKind::Axis&&virtual_config.ports[0].pad[axis<4?12:6].code==axis&&virtual_config.ports[0].pad[axis<4?12:6].sign==(((axis==1||axis==3)?-input_value:input_value)<0?-1:1)&&!pad_doc->GetElementById(axis<4?"bind-12-0":"bind-6-0")->IsClassSet("editing"),"{}");
          SDL_SetJoystickVirtualAxis(capture_joystick,axis,axis<4?0:SDL_JOYSTICK_AXIS_MIN);frame("capture-axis-release-"+std::to_string(axis),modal);
        }
        // Real SDL readings from the fixture drive the preview. No simulation
        // buttons or invented samples are part of the production interface.
        virtual_config.ports[0].pad=narutorise::controls::DefaultPad();virtual_config.ports[0].rumble={.4,.7};
        narutorise::controls::Publish(virtual_config);menu.HidePanel();
        {
          auto nav_config=virtual_config;int saves=0;
          menu.ShowControls(nav_config,[&](const auto& next){nav_config=next;++saves;return true;},[]{},"F2");frame("pad-preview-route-open",modal);
          for(int i=0;i<5;++i)pad_key(SDL_GAMEPAD_BUTTON_DPAD_DOWN,"pad-preview-route-category-"+std::to_string(i));
          check("pad-preview-category-reached",focused_id()=="category-deadzones","{}");
          pad_key(SDL_GAMEPAD_BUTTON_SOUTH,"pad-preview-enter");
          check("pad-preview-fields-reached",focused_id()=="deadzone-toggle"&&saves==0,"{}");
          auto* preview_doc=visible_document("controls-window");const auto unchanged=nav_config;
          const int samples[]={28000,-28000,-28000,28000,12000,18000};
          for(int axis=0;axis<6;++axis)SDL_SetJoystickVirtualAxis(capture_joystick,axis,samples[axis]);
          frame("pad-preview-full-range",modal);frame("pad-preview-held",modal);
          check("pad-preview-inputs-preserve-focus-and-config",focused_id()=="deadzone-toggle"&&nav_config==unchanged&&saves==0&&preview_doc->GetElementById("player-0")->IsClassSet("selected"),"{}");
          check("pad-preview-both-sticks-and-triggers-live",preview_doc->GetElementById("raw-0")->GetProperty("left")->Get<float>()>85&&preview_doc->GetElementById("raw-1")->GetProperty("left")->Get<float>()<15&&preview_doc->GetElementById("trigger-raw-0")->GetProperty("width")->Get<float>()>60&&preview_doc->GetElementById("trigger-raw-1")->GetProperty("width")->Get<float>()>70,"{}");
          pad_key(SDL_GAMEPAD_BUTTON_DPAD_DOWN,"pad-preview-navigate-adjustment");
          check("pad-preview-dpad-reaches-adjustment",focused_id()=="minus-deadzone-0"||focused_id()=="plus-deadzone-0","{\"focus\":\""+focused_id()+"\"}");
          pad_key(SDL_GAMEPAD_BUTTON_DPAD_RIGHT,"pad-preview-adjust");
          check("pad-preview-adjusts-with-stick-held",saves==1&&std::abs(nav_config.ports[0].deadzone[0]-unchanged.ports[0].deadzone[0]-.01)<.001,"{}");
          pad_key(SDL_GAMEPAD_BUTTON_EAST,"pad-preview-back");
          check("pad-preview-back-to-category",focused_id()=="category-deadzones","{}");
          pad_key(SDL_GAMEPAD_BUTTON_DPAD_UP,"pad-preview-leave-category");
          pad_key(SDL_GAMEPAD_BUTTON_SOUTH,"pad-preview-enter-rumble");
          const auto held_focus=focused_id();frame("pad-preview-exit-stick-held",modal);
          check("pad-preview-exit-no-stray-stick-navigation",held_focus=="minus-rumble-0"&&focused_id()==held_focus&&saves==1,"{}");
          for(int axis=0;axis<6;++axis)SDL_SetJoystickVirtualAxis(capture_joystick,axis,axis<4?0:SDL_JOYSTICK_AXIS_MIN);
          frame("pad-preview-neutral",modal);
          SDL_SetJoystickVirtualAxis(capture_joystick,SDL_GAMEPAD_AXIS_LEFTY,28000);frame("pad-preview-navigation-restored",modal);
          check("pad-preview-stick-navigation-after-neutral",focused_id()=="minus-rumble-1"&&saves==1,"{\"focus\":\""+focused_id()+"\"}");
          SDL_SetJoystickVirtualAxis(capture_joystick,SDL_GAMEPAD_AXIS_LEFTY,0);frame("pad-preview-route-release",modal);
          menu.HidePanel();
        }
        menu.ShowControls(virtual_config,[&](const auto& next){virtual_config=next;narutorise::controls::Publish(next);return true;},[]{},"F2");frame("adjustments-window",modal);
        pad_doc=visible_document("controls-window");
        focus_id("category-deadzones");key(rex::ui::VirtualKey::kReturn);frame("deadzone-preview",modal);
        SDL_SetJoystickVirtualAxis(capture_joystick,SDL_GAMEPAD_AXIS_LEFTX,2000);frame("deadzone-drift-reading",modal);
        auto* raw_dot=pad_doc->GetElementById("raw-0");auto* effective_dot=pad_doc->GetElementById("effective-0");
        check("deadzone-preview-ignores-real-drift",raw_dot&&effective_dot&&raw_dot->GetProperty("left")->Get<float>()>50&&std::abs(effective_dot->GetProperty("left")->Get<float>()-50)<.01,"{}");
        if(raw_dot){const auto size=raw_dot->GetParentNode()->GetBox().GetSize();const auto parent=raw_dot->GetParentNode()->GetAbsoluteOffset();const auto point=raw_dot->GetAbsoluteOffset();
          check("deadzone-graph-layout-bounded",size.x>100&&size.x<120&&size.y>100&&size.y<120&&point.x>=parent.x&&point.x<parent.x+size.x&&point.y>=parent.y&&point.y<parent.y+size.y,"{}");}
        SDL_SetJoystickVirtualAxis(capture_joystick,SDL_GAMEPAD_AXIS_LEFTX,12000);SDL_SetJoystickVirtualAxis(capture_joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-25000);frame("deadzone-movement-reading",modal);
        check("deadzone-preview-real-movement",effective_dot->GetProperty("left")->Get<float>()>50&&raw_dot->GetProperty("left")->Get<float>()>effective_dot->GetProperty("left")->Get<float>(),"{}");
        std::ofstream(std::string(argv[1])+".deadzone-preview-1280x720.bgra",std::ios::binary).write(reinterpret_cast<const char*>(modal.data()),std::streamsize(modal.size()));
        focus_id("deadzone-toggle");key(rex::ui::VirtualKey::kReturn);frame("deadzone-disabled",modal);
        raw_dot=pad_doc->GetElementById("raw-0");effective_dot=pad_doc->GetElementById("effective-0");
        check("deadzone-toggle-effective",!virtual_config.ports[0].deadzones&&std::abs(raw_dot->GetProperty("left")->Get<float>()-effective_dot->GetProperty("left")->Get<float>())<.01,"{}");
        focus_id("deadzone-toggle");key(rex::ui::VirtualKey::kReturn);frame("deadzone-enabled",modal);
        const auto right_deadzone=virtual_config.ports[0].deadzone[1];
        focus_id("plus-deadzone-0");key(rex::ui::VirtualKey::kReturn);frame("deadzone-left-adjusted",modal);
        check("deadzone-adjust-isolated",std::abs(virtual_config.ports[0].deadzone[0]-.16)<.001&&virtual_config.ports[0].deadzone[1]==right_deadzone,"{}");
        SDL_SetJoystickVirtualAxis(capture_joystick,SDL_GAMEPAD_AXIS_LEFTX,0);SDL_SetJoystickVirtualAxis(capture_joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,SDL_JOYSTICK_AXIS_MIN);frame("deadzone-restored-rest",modal);
        focus_id("device-category");key(rex::ui::VirtualKey::kReturn);frame("identify-device-list",modal);
        auto before_identify=virtual_config;
        focus_id("identify-0");key(rex::ui::VirtualKey::kReturn);frame("identify-running",modal);
        check("identify-brief-real-backend-no-assignment",narutorise::controls::Hardware().RumbleActive()&&rumble_probe.low>0&&virtual_config==before_identify,"{}");
        Sleep(300);frame("identify-finished",modal);
        check("identify-stops",!narutorise::controls::Hardware().RumbleActive()&&rumble_probe.low==0&&rumble_probe.high==0,"{}");
        focus_id("category-5");key(rex::ui::VirtualKey::kReturn);frame("rumble-page",modal);
        focus_id("rumble-test");key(rex::ui::VirtualKey::kReturn);frame("rumble-start",modal);
        check("rumble-backend-intensities",narutorise::controls::Hardware().RumbleActive()&&std::abs(int(rumble_probe.low)-26214)<=1&&std::abs(int(rumble_probe.high)-45874)<=1,"{}");
        check("rumble-adjustments-disabled",pad_doc->GetElementById("plus-rumble-0")->HasAttribute("disabled"),"{}");
        focus_id("rumble-test");key(rex::ui::VirtualKey::kReturn);frame("rumble-stopped",modal);
        check("rumble-stop-button",!narutorise::controls::Hardware().RumbleActive()&&!rumble_probe.low&&!rumble_probe.high,"{}");
        focus_id("rumble-test");key(rex::ui::VirtualKey::kReturn);frame("rumble-restart",modal);
        focus_id("category-1");key(rex::ui::VirtualKey::kReturn);frame("rumble-stop-on-category",modal);
        check("rumble-stops-on-leave",!rumble_probe.low&&!rumble_probe.high&&!narutorise::controls::Hardware().RumbleActive(),"{}");
        focus_id("category-5");key(rex::ui::VirtualKey::kReturn);frame("rumble-player-page",modal);
        focus_id("rumble-test");key(rex::ui::VirtualKey::kReturn);frame("rumble-player-start",modal);
        key(rex::ui::VirtualKey::kNext);frame("rumble-player-changed",modal);
        check("rumble-stops-on-player-change",!rumble_probe.low&&!rumble_probe.high&&!narutorise::controls::Hardware().RumbleActive(),"{}");
        key(rex::ui::VirtualKey::kPrior);frame("rumble-player-return",modal);
        focus_id("category-5");key(rex::ui::VirtualKey::kReturn);frame("rumble-focus-page",modal);
        focus_id("rumble-test");key(rex::ui::VirtualKey::kReturn);frame("rumble-focus-start",modal);
        bool input_focused=false;menu.SetInputActive([&]{return input_focused;});frame("rumble-focus-lost",modal);
        check("rumble-stops-on-focus-loss",!rumble_probe.low&&!rumble_probe.high&&!narutorise::controls::Hardware().RumbleActive(),"{}");
        input_focused=true;menu.SetInputActive({});
        focus_id("rumble-test");key(rex::ui::VirtualKey::kReturn);frame("rumble-expiry-start",modal);
        Sleep(5100);frame("rumble-expired",modal);
        check("rumble-has-deadline",!rumble_probe.low&&!rumble_probe.high&&!narutorise::controls::Hardware().RumbleActive(),"{}");
        rumble_probe.fail=true;focus_id("rumble-test");key(rex::ui::VirtualKey::kReturn);frame("rumble-backend-failure",modal);
        check("rumble-failure-visible",!narutorise::controls::Hardware().RumbleActive()&&pad_doc->GetElementById("controls-status")->GetInnerRML()==narutorise::menu::Text("c-rumble-failed"),"{}");rumble_probe.fail=false;
        focus_id("rumble-test");key(rex::ui::VirtualKey::kReturn);frame("rumble-close-start",modal);menu.HidePanel();
        check("rumble-stops-on-close",!rumble_probe.low&&!rumble_probe.high&&!narutorise::controls::Hardware().RumbleActive(),"{}");
        const auto hardware_key=virtual_config.ports[0].device,hardware_name=virtual_config.ports[0].device_name;
        for(int restored_player:{0,2}){
          virtual_config.ports[restored_player].pad[0]={narutorise::controls::SourceKind::Button,0x8000};
          virtual_config.ports[restored_player].keys[0]="F5";virtual_config.ports[restored_player].sensitivity=2.42;
          virtual_config.ports[restored_player].deadzone[1]=.42;virtual_config.ports[restored_player].rumble={.12,.34};
          narutorise::controls::Assign(virtual_config,size_t(restored_player),hardware_key,hardware_name);
          narutorise::controls::Publish(virtual_config);int restore_saves=0;
          menu.ShowControls(virtual_config,[&](const auto& next){++restore_saves;virtual_config=next;narutorise::controls::Publish(next);return true;},[]{},"F2");frame("restore-window-"+std::to_string(restored_player),modal);
          if(restored_player){focus_id("player-2");key(rex::ui::VirtualKey::kReturn);frame("restore-player-three",modal);}
          pad_doc=visible_document("controls-window");const auto before_restore=virtual_config;
          focus_id("category-restore");key(rex::ui::VirtualKey::kReturn);frame("restore-confirmation-"+std::to_string(restored_player),modal);
          key(rex::ui::VirtualKey::kReturn);frame("restore-default-cancel-"+std::to_string(restored_player),modal);
          check("restore-cancel-preserves-state-"+std::to_string(restored_player),virtual_config==before_restore&&restore_saves==0,"{}");
          focus_id("category-restore");key(rex::ui::VirtualKey::kReturn);frame("restore-reopen-"+std::to_string(restored_player),modal);
          focus_id("restore-confirm");key(rex::ui::VirtualKey::kReturn);frame("restore-applied-"+std::to_string(restored_player),modal);
          auto expected=narutorise::controls::Port{};expected.device=hardware_key;expected.device_name=hardware_name;
          bool isolated=virtual_config.ports[restored_player]==expected;
          for(int i=0;i<4;++i)if(i!=restored_player)isolated&=virtual_config.ports[i]==before_restore.ports[i];
          check("restore-both-banks-isolated-keeps-device-"+std::to_string(restored_player),isolated&&restore_saves==1,"{}");
          menu.HidePanel();
        }
        narutorise::controls::Assign(virtual_config,0,hardware_key,hardware_name);narutorise::controls::Publish(virtual_config);
        menu.ShowControls(virtual_config,[&](const auto& next){virtual_config=next;narutorise::controls::Publish(next);return true;},[]{},"F2");frame("hotplug-return-controls",modal);
        pad_doc=visible_document("controls-window");focus_id("category-3");key(rex::ui::VirtualKey::kReturn);frame("hotplug-return-triggers",modal);
        // Two available devices with the same model name must remain distinct.
        const auto twin_id=SDL_AttachVirtualJoystick(&capture_pad);auto* twin=SDL_OpenJoystick(twin_id);
        frame("same-model-second-connected",modal);
        const auto twins=narutorise::controls::Hardware().Devices();const narutorise::controls::Device* second_device=nullptr;
        for(const auto& d:twins->devices)if(d.pad&&SDL_GetGamepadID(d.pad)==twin_id)second_device=&d;
        check("same-model-distinct-identities",second_device&&second_device->key!=virtual_config.ports[0].device&&second_device->name!=virtual_config.ports[0].device_name,"{}");
        if(second_device){
          narutorise::controls::Assign(virtual_config,1,second_device->key,second_device->name);narutorise::controls::Publish(virtual_config);
          menu.HidePanel();menu.ShowControls(virtual_config,[&](const auto& next){virtual_config=next;narutorise::controls::Publish(next);return true;},[]{},"F2");frame("same-model-controls",modal);
          pad_doc=visible_document("controls-window");focus_id("category-1");key(rex::ui::VirtualKey::kReturn);frame("same-model-buttons",modal);
          key(rex::ui::VirtualKey::kNext);frame("same-model-next-player",modal);
          check("player-switch-preserves-available-category",pad_doc->GetElementById("bind-0-0")&&pad_doc->GetElementById("player-1")->IsClassSet("selected"),"{}");
          key(rex::ui::VirtualKey::kPrior);frame("same-model-return-player",modal);
          focus_id("category-3");key(rex::ui::VirtualKey::kReturn);frame("same-model-return-triggers",modal);
        }
        SDL_CloseJoystick(twin);SDL_DetachVirtualJoystick(twin_id);frame("same-model-second-disconnected",modal);
        focus_id("category-5");key(rex::ui::VirtualKey::kReturn);frame("rumble-disconnect-page",modal);
        focus_id("rumble-test");key(rex::ui::VirtualKey::kReturn);frame("rumble-disconnect-start",modal);
        check("rumble-active-before-disconnect",narutorise::controls::Hardware().RumbleActive(),"{}");
        SDL_CloseJoystick(capture_joystick);SDL_DetachVirtualJoystick(capture_id);capture_joystick=nullptr;frame("rumble-disconnected",modal);
        // SDL retires the disconnected backend: no new motor write can reach
        // it. Verify cancellation and invalidation; physical motor shutdown
        // requires hardware, rather than the observer's last historical call.
        check("rumble-stops-on-disconnect",!narutorise::controls::Hardware().RumbleActive()&&!narutorise::controls::FindDevice(*narutorise::controls::Hardware().Devices(),virtual_config.ports[0].device),"{}");
        capture_id=SDL_AttachVirtualJoystick(&capture_pad);capture_joystick=SDL_OpenJoystick(capture_id);frame("rumble-reconnected",modal);
        focus_id("device-category");key(rex::ui::VirtualKey::kReturn);frame("rumble-reassign-device-page",modal);
        focus_id("device-0");key(rex::ui::VirtualKey::kReturn);frame("rumble-reassign-device",modal);
        focus_id("category-3");key(rex::ui::VirtualKey::kReturn);frame("rumble-return-triggers",modal);
        auto before_disconnect=virtual_config;
        focus_id("bind-6-0");key(rex::ui::VirtualKey::kReturn);frame("hotplug-capture-start",modal);
        SDL_CloseJoystick(capture_joystick);SDL_DetachVirtualJoystick(capture_id);capture_joystick=nullptr;
        frame("hotplug-capture-disconnect",modal);
        check("hotplug-cancels-capture-preserves-profile",virtual_config==before_disconnect&&!pad_doc->GetElementById("bind-6-0")->IsClassSet("editing"),"{}");
        const auto reconnected_id=SDL_AttachVirtualJoystick(&capture_pad);auto* reconnected=SDL_OpenJoystick(reconnected_id);
        frame("hotplug-reconnected-device",modal);
        check("hotplug-does-not-alias-old-handle",narutorise::controls::FindDevice(*narutorise::controls::Hardware().Devices(),virtual_config.ports[0].device)==nullptr,"{}");
        focus_id("device-category");key(rex::ui::VirtualKey::kReturn);frame("hotplug-device-page",modal);
        focus_id("device-0");key(rex::ui::VirtualKey::kReturn);frame("hotplug-device-reassigned",modal);
        key(rex::ui::VirtualKey::kNext);frame("hotplug-target-player",modal);
        focus_id("device-0");key(rex::ui::VirtualKey::kReturn);frame("hotplug-transfer-start",modal);
        check("hotplug-transfer-prompt",pad_doc->GetElementById("transfer-confirm")!=nullptr,"{}");
        auto before_stale=virtual_config;
        SDL_CloseJoystick(reconnected);SDL_DetachVirtualJoystick(reconnected_id);frame("hotplug-transfer-invalidated",modal);
        check("hotplug-invalidates-stale-transfer",!pad_doc->GetElementById("transfer-confirm")&&virtual_config==before_stale,"{}");
        menu.HidePanel();
      }
      if(capture_joystick){SDL_CloseJoystick(capture_joystick);SDL_DetachVirtualJoystick(capture_id);}narutorise::controls::Hardware().Poll("sdl",true);
    }
    menu.HidePanel();
    settings_model.Open(menu,3,false,{},[](const std::string&){},[]{});frame("menu-bind-controls",modal);
    focus_id("bind_game_menu");key(rex::ui::VirtualKey::kReturn);frame("menu-bind-capture",modal);
    key(rex::ui::VirtualKey::kF2);frame("menu-bind-saved",modal);
    focus_id("game_menu_chord");key(rex::ui::VirtualKey::kRight);frame("menu-chord-saved",modal);
    saved=toml::parse_file(settings_path.string());
    check("menu-shortcuts-persisted",saved["bind_game_menu"].value<std::string>()=="F2"&&saved["game_menu_chord"].value<std::string>()=="LB+RB+Start","{}");
    key(rex::ui::VirtualKey::kNext);frame("menu-advanced",modal);
    focus_id("show_shader_indicator");key(rex::ui::VirtualKey::kRight);frame("menu-shaders-saved",modal);
    saved=toml::parse_file(settings_path.string());
    check("shader-indicator-option-persisted",saved["show_shader_indicator"].value<bool>()==false,"{}");
    menu.HidePanel();

    const auto preserved_configuration=[&]{std::ifstream in(settings_path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});}();
    const auto effective_before_failure=*narutorise::controls::Current();
    std::ofstream(settings_path)<<"malformed = [";
    settings_model.Open(menu,3,false,{},[](const std::string&){},[]{});frame("invalid-config-settings",modal);
    focus_id("configure-controls");key(rex::ui::VirtualKey::kReturn);frame("invalid-config-controls",modal);
    focus_id("category-1");key(rex::ui::VirtualKey::kReturn);frame("invalid-config-bindings",modal);
    focus_id("bind-0-0");key(rex::ui::VirtualKey::kReturn);frame("invalid-config-capture",modal);
    key(rex::ui::VirtualKey::kF5);frame("invalid-config-save-refused",modal);
    {std::ifstream in(settings_path);const std::string actual(std::istreambuf_iterator<char>(in),{});
      check("invalid-config-not-overwritten-or-published",actual=="malformed = ["&&*narutorise::controls::Current()==effective_before_failure&&visible_document("dialog-title"),"{}");}
    menu.HideDialog();menu.HidePanel();std::ofstream(settings_path)<<preserved_configuration;settings_model.Reload();
    {
      const auto live_dir = settings_dir / "live"; std::filesystem::create_directories(live_dir);
      std::ofstream(live_dir / "narutorise.toml") << "brightness = 0.5\n";
      narutorise::menu::Settings live;
      live.Initialize(live_dir);
      narutorise::menu::Settings::LiveResult result;
      uint64_t ticket = 0; int changes = 0;
      live.SetLiveSettings([&](const std::string& key, const GameConfig&) { return key == "brightness" ? ++ticket : 0; },
                           [&] { return result; });
      live.Open(menu, 1, true, {}, [&](const std::string&) { ++changes; }, [] {});
      frame("live-settings-open", modal);
      auto* panel = visible_document("panel-title");
      check("live-restart-labels", panel && panel->GetElementById("brightness")->GetInnerRML().find("class=\"restart\"") == std::string::npos &&
          panel->GetElementById("native_backend")->GetInnerRML().find("restart") != std::string::npos, "{}");
      focus_id("brightness"); key(rex::ui::VirtualKey::kRight); frame("live-settings-pending", modal);
      check("live-value-waits-for-renderer", ticket == 1 && changes == 0 && live.config().brightness == .5 &&
          toml::parse_file((live_dir / "narutorise.toml").string())["brightness"].value_or(0.0) > .5, "{}");
      focus_id("frame_rate"); result = {ticket, true}; live.PollLive(); frame("live-settings-confirmed", modal);
      check("live-confirmation-preserves-navigation", changes == 1 && std::abs(live.config().brightness - .51) < .0001 &&
          menu.PanelFocus() == "frame_rate", "{}");
      focus_id("brightness"); key(rex::ui::VirtualKey::kRight); frame("live-settings-refused-pending", modal);
      result = {ticket, false}; live.PollLive(); frame("live-settings-refused", modal);
      check("live-refusal-restores-saved-value", changes == 1 && std::abs(live.config().brightness - .51) < .0001 &&
          std::abs(toml::parse_file((live_dir / "narutorise.toml").string())["brightness"].value_or(0.0) - .51) < .0001 &&
          visible_document("dialog-title"), "{}");
      menu.HideDialog(); live.Open(menu, 1, true, {}, [&](const std::string&) { ++changes; }, [] {});
      frame("live-settings-reopen", modal);
      focus_id("brightness"); key(rex::ui::VirtualKey::kRight); frame("live-settings-close-pending", modal);
      menu.HidePanel(); const auto closed_revision = menu.ViewRevision();
      result = {ticket, true}; live.PollLive(); frame("live-settings-completed-after-close", modal);
      check("late-live-result-does-not-reopen-panel", !menu.PanelShown() && menu.ViewRevision() == closed_revision && changes == 2, "{}");
      settings_model.Reload();
    }
    {narutorise::menu::Settings legacy_renderer_settings;legacy_renderer_settings.Initialize(settings_dir,false);
      check("legacy-renderer-keeps-sdk-profile-policy",!rex::input::mnk::GetProfile()&&!narutorise::controls::Current()->version,"{}");
      settings_model.Reload();}
    for(const auto& language:Localization::GetLanguages()){
      Localization::SetLanguage(language.language);
      menu.ShowControls(settings_model.config().controls,[](const auto&){return false;},[]{},"F2");frame("controls-language-window-"+std::string(language.code),modal);
      focus_id("category-1");key(rex::ui::VirtualKey::kReturn);frame("controls-language-"+std::string(language.code),modal);
      auto* translated=visible_document("controls-window");
      check("controls-seven-language-"+std::string(language.code),translated&&translated->GetElementById("controls-back")->GetInnerRML().find(narutorise::menu::Text("c-controls"))!=std::string::npos&&!std::string(narutorise::menu::Text("c-capture-waiting")).empty(),"{}");
      std::ofstream(std::string(argv[1])+".controls-"+language.code+"-1280x720.bgra",std::ios::binary).write(reinterpret_cast<const char*>(modal.data()),std::streamsize(modal.size()));
      menu.HidePanel();
    }
    Localization::SetLanguage(Localization::Language::English);

    {
      const auto apply_dir = settings_dir / "apply";
      std::filesystem::create_directories(apply_dir);
      std::ofstream(apply_dir / "narutorise.toml")
          << "frame_rate=30\nnative_backend='d3d12'\nskip_intro_videos=false\nmnk_mode=false\n";
      rex::cvar::SetFlagFromCommandLine("native_backend", "vulkan");
      rex::cvar::SetFlagFromCommandLine("skip_intro_videos", "true");
      rex::cvar::SetFlagFromCommandLine("mnk_mode", "true");
      narutorise::menu::Settings apply_settings;
      apply_settings.Initialize(apply_dir);
      check("migration-uses-effective-keyboard-override",
            narutorise::controls::Current()->ports[0].device == "keyboard", "{}");
      check("setup-without-edit-keeps-overrides", apply_settings.ApplyBeforeGame() &&
            rex::cvar::GetFlagByName("native_backend") == "vulkan" &&
            rex::cvar::GetFlagByName("skip_intro_videos") == "true", "{}");
      apply_settings.Open(menu, 1, false, {}, {}, {});
      frame("apply-settings-display", modal);
      focus_id("frame_rate");
      key(rex::ui::VirtualKey::kRight); frame("apply-frame-rate-60", modal);
      key(rex::ui::VirtualKey::kRight); frame("apply-frame-rate-120", modal);
      check("setup-integer-edit-applied-on-play", apply_settings.ApplyBeforeGame() &&
            rex::cvar::GetFlagByName("frame_rate") == "120", "{}");
      check("editing-another-option-keeps-overrides",
            rex::cvar::GetFlagByName("native_backend") == "vulkan" &&
            rex::cvar::GetFlagByName("skip_intro_videos") == "true", "{}");
      apply_settings.Open(menu, 0, false, {}, {}, {});
      frame("apply-settings-game", modal);
      focus_id("skip_intro_videos");
      key(rex::ui::VirtualKey::kRight); frame("apply-explicit-bool-on", modal);
      key(rex::ui::VirtualKey::kRight); frame("apply-explicit-bool-edit", modal);
      check("explicit-menu-edit-can-replace-cli", apply_settings.ApplyBeforeGame() &&
            rex::cvar::GetFlagByName("skip_intro_videos") == "false", "{}");
      menu.HidePanel();
    }

    // The UI screen before the game: the engine goes, the screen's own engine
    // on the window presents the layer and asks for frames as the Swaps do;
    // closed, the game's engine (a new one) gets everything.
    close("first-", engine);
    std::atomic<int> requests{0};
    layer.SetFrameRequest([&] { ++requests; });
    {
      ui::Screen screen(layer);
      std::string opened;
      if (check("screen-open", screen.Open(first, TestRenderMode(), opened), opened.empty() ? "{}" : opened)) {
        for (int i = 0; i < 50 && requests < 10; ++i) Sleep(20);
        check("screen-frames", requests >= 10, "{\"requests\":" + std::to_string(requests.load()) + "}");
        screen.Close();
        check("screen-closed", !screen.IsOpen(), "{}");
      }
    }
    layer.SetFrameRequest({});
    engine = open("second-");
    std::vector<uint8_t> back;
    if (frame("setup-handover", back)) {
      const auto dlc = at(back, 500, 455), art = at(back, 20, 20);
      check("setup-handover-ui", Near(dlc, {245,138,53}, 4) && Near(art, {8,15,27}, 4),
            "{\"dlc\":" + Json(dlc) + ",\"art\":" + Json(art) + "}");
    }
    menu.HideSetup();
    check("setup-hidden", !menu.Shown(), "{}");
    {
      // Complete real filesystem work after its UI destination has closed.
      const auto source = settings_dir / "async-source";
      const auto user = settings_dir / "async-user";
      std::filesystem::create_directories(source);
      std::ofstream(source / "content.bin") << "late DLC content";
      menu.HideSetup();
      menu.ShowOverlay({}, {}, {}, {});
      narutorise::menu::Dlc dlc;
      dlc.Open(menu, settings_dir / "async-product", user, nullptr,
               [](auto action) { action(); }, {});
      dlc.InstallFolder(source);
      menu.HideOverlay();
      dlc.Wait(); dlc.Poll();
      frame("async-dlc-finished-after-close", modal);
      check("late-dlc-result-does-not-reopen-ui", !menu.OverlayShown() &&
            !menu.PanelShown() && !visible_document("dialog-title"), "{}");
      check("closed-menu-still-finishes-authorized-copy", std::filesystem::is_regular_file(
            user / "0000000000000000/555307E5/00000002/async-source/content.bin"), "{}");
      menu.ShowOverlay({}, {}, {}, {});
      dlc.Open(menu, settings_dir / "async-product", user, nullptr,
               [](auto action) { action(); }, {});
      dlc.InstallFolder(source);
      settings_model.Open(menu, 0, true, {}, {}, {});
      dlc.Wait(); dlc.Poll();
      frame("async-dlc-finished-on-another-page", modal);
      check("late-dlc-result-does-not-replace-another-page", menu.PanelShown() &&
            !visible_document("dialog-title"), "{}");
      menu.HideOverlay();
    }

    {
      const auto product = settings_dir / "named-dlc-product";
      const auto user = settings_dir / "named-dlc-user";
      const std::string package = "AEFD8711E05CF01634F0053A5AB2E3D2520E1F0A55";
      std::filesystem::create_directories(product / "dlc");
      rex::filesystem::StfsHeader metadata{};
      metadata.header.magic = rex::filesystem::XContentPackageType::kLive;
      metadata.metadata.set_display_name(rex::system::XLanguage::kEnglish, u"Choji & Temari");
      std::ofstream(product / "dlc" / package, std::ios::binary)
          .write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
      narutorise::menu::Dlc dlc;
      dlc.Open(menu, product, user, nullptr, [](auto action) { action(); }, {});
      frame("dlc-pending-name", modal);
      auto* document = visible_document("options");
      auto* pending = document ? document->GetElementById("staged-1") : nullptr;
      check("dlc-pending-shows-package-name", pending &&
            pending->GetInnerRML().find("Choji &amp; Temari") != std::string::npos &&
            pending->GetInnerRML().find(package) == std::string::npos, "{}");
      menu.HidePanel();
      // Reopen after the runtime has extracted the content and written its header.
      const auto root = launcher_content::GetDlcRoot();
      std::filesystem::create_directories(root / package);
      std::ofstream(root / package / "content.bin") << "installed content";
      std::filesystem::create_directories(root.parent_path() / "Headers/00000002");
      rex::system::xam::XCONTENT_AGGREGATE_DATA header{};
      header.content_type = rex::system::XContentType::kMarketplaceContent;
      header.title_id = 0x555307E5;
      header.set_file_name(package); header.set_display_name(u"Choji & Temari");
      std::ofstream(root.parent_path() / "Headers/00000002" / (package + ".header"), std::ios::binary)
          .write(reinterpret_cast<const char*>(&header), sizeof(header));
      narutorise::menu::Dlc reopened;
      reopened.Open(menu, product, user, nullptr, [](auto action) { action(); }, {});
      frame("dlc-installed-name", modal);
      document = visible_document("options");
      auto* installed = document ? document->GetElementById("dlc-0") : nullptr;
      check("dlc-installed-keeps-name-without-pending-duplicate", installed && installed->IsClassSet("good") &&
            installed->GetInnerRML().find("Choji &amp; Temari") != std::string::npos &&
            !document->GetElementById("staged-1") && !document->GetElementById("dlc-empty"), "{}");
      menu.HidePanel();
    }

    int opened = 0, overlay_settings = 0, confirmed_exit = 0, canceled_exit = 0;
    menu.EnableGameMenu("F2", "Back+Start", [&] {
      ++opened;
      if (menu.OverlayShown()) menu.HideOverlay();
      else menu.ShowOverlay([&] { ++overlay_settings; }, {}, [&] {
        menu.ShowDialog("Exit game", "Exit to the desktop?", {{"Exit", [&] { ++confirmed_exit; }},
                                                               {"Cancel", [&] { ++canceled_exit; }}});
      }, [&] { menu.HideOverlay(); });
    });
    key(rex::ui::VirtualKey::kF2); frame("overlay", modal);
    check("overlay-hotkey-remapped", menu.OverlayShown() && opened == 1, "{}");
    check("overlay-no-dimming", Near(at(modal, 800, 400), {0,255,0}, 1), Json(at(modal, 800, 400)));
    std::ofstream(std::string(argv[1]) + ".overlay-1280x720.bgra", std::ios::binary)
        .write(reinterpret_cast<const char*>(modal.data()), std::streamsize(modal.size()));
    key(rex::ui::VirtualKey::kReturn); frame("overlay-settings", modal);
    check("overlay-shared-settings-action", overlay_settings == 1, "{}");
    key(rex::ui::VirtualKey::kRight); key(rex::ui::VirtualKey::kRight);
    key(rex::ui::VirtualKey::kReturn); frame("overlay-exit-modal", modal);
    key(rex::ui::VirtualKey::kReturn); frame("overlay-exit-canceled", modal);
    check("overlay-exit-cancel-default", canceled_exit == 1 && confirmed_exit == 0, "{}");
    key(rex::ui::VirtualKey::kF2); frame("overlay-closed", modal);
    check("overlay-clears-persistent-frame", !menu.OverlayShown() && Near(at(modal, 300,20), {0,255,0}, 1), "{}");
    int release_pages = 0;
    menu.ShowShortcutHint("Pressione  F1  ou  Back + Start  para abrir o menu");
    frame("shortcut-hint", modal);
    check("shortcut-hint-visible", menu.HintShown(), "{}");
    std::ofstream(std::string(argv[1]) + ".hint-1280x720.bgra", std::ios::binary)
        .write(reinterpret_cast<const char*>(modal.data()), std::streamsize(modal.size()));
    Sleep(2600); frame("shortcut-hint-expired", modal);
    check("shortcut-hint-expires-at-2-5s", !menu.HintShown(), "{}");
    menu.Notify("0.5.0-beta · see Settings › Advanced", "Open release", [&] { ++release_pages; }, "New version available");
    menu.SetShadersVisible(true, 214, .4f);
    frame("notifications", modal);
    check("notification-shown", menu.Shown(), "{}");
    std::ofstream(std::string(argv[1]) + ".notification-1280x720.bgra", std::ios::binary)
        .write(reinterpret_cast<const char*>(modal.data()), std::streamsize(modal.size()));
    check("shader-progress-orange", Near(at(modal, 80, 688), orange, 4), Json(at(modal,80,688)));
    rex::ui::MouseEvent notice_move(nullptr, rex::ui::MouseEvent::Button::kNone, 1100, 100);
    rex::ui::MouseEvent notice_click(nullptr, rex::ui::MouseEvent::Button::kLeft, 1100, 100);
    menu.OnMouseMove(notice_move); menu.OnMouseDown(notice_click); menu.OnMouseUp(notice_click);
    frame("notification-open", modal);
    check("notification-release-action", release_pages == 1, "{}");
    SetWindowPos(first, nullptr, 0, 0, int(kWidth), int(kHeight), SWP_NOZORDER | SWP_NOACTIVATE);
  };

  ui::Layer layer;
  {
    ui::ImmediateDrawer drawer(layer);
    narutorise::menu::Ui menu;
    check("ui-initialize", menu.Initialize(drawer, nullptr), "{}");
    rex::ui::AppUIDrawContext context(kWidth, kHeight);
    auto swap = [&](const std::string& kind, std::vector<uint8_t>& pixels) {
      check(kind + "-deliver", layer.Deliver(*engine), "{}");
      uint32_t w = 0, h = 0;
      return check(kind + "-present", engine->Run([&](Engine& e, std::string& out) {
        uint32_t slot = 0;
        return e.StageFront(kSurface, slot, out) && e.Present(slot, true, pixels, w, h, out);
      }, evidence) && w == kWidth && h == kHeight, evidence);
    };
    menu.SetFpsVisible(true);
    for (int i = 0; i < 30; ++i) menu.CountFrame();
    menu.Draw(context);
    std::vector<uint8_t> shown;
    if (swap("fps", shown)) {
      std::ofstream(std::string(argv[1]) + ".fps-640x360.bgra", std::ios::binary)
          .write(reinterpret_cast<const char*>(shown.data()), std::streamsize(shown.size()));
      // The panel (its left padding): #0E1220 at 0xD9 over green; the border: #F2701F; the text:
      // brighter than the panel.
      const auto panel = Pixel(shown, 6, 25), border = Pixel(shown, 1, 25), outside = Pixel(shown, 300, 200);
      check("panel", Near(panel, {12, 53, 27}, 4), Json(panel));
      check("border", Near(border, {242, 112, 31}, 4), Json(border));
      check("outside", Near(outside, {0, 255, 0}, 1), Json(outside));
      uint32_t text = 0;
      for (uint32_t y = 8; y < 60; ++y)
        for (uint32_t x = 14; x < 120; ++x) {
          const auto p = Pixel(shown, x, y);
          text += p[0] + p[1] + p[2] > 300;
        }
      check("text", text > 40, "{\"bright_pixels\":" + std::to_string(text) + "}");
    }
    menu.SetFpsVisible(false);
    menu.Draw(context);
    std::vector<uint8_t> hidden;
    if (swap("hidden", hidden)) {
      bool green = true;
      for (uint32_t y = 0; y < kHeight && green; ++y)
        for (uint32_t x = 0; x < kWidth && green; ++x) green = Near(Pixel(hidden, x, y), {0, 255, 0}, 1);
      check("hidden-green", green, "{}");
    }
    setup(menu, layer, engine, window);
    menu.Shutdown();
  }
  if (!engine) return 6;
  layer.Deliver(*engine);  // The releases of the UI's textures.
  std::string counts;
  engine->Run([](Engine& e, std::string& out) { out = e.ResourceCounts(); return true; }, counts);
  check("released", counts.find("\"ui_textures\":0,") != std::string::npos, counts);
  close("", engine);
  DestroyWindow(window);
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << "}\n";
  return passed ? 0 : 1;
}
