// The RmlUi of the game (ui.h).
#include "menu/ui.h"
#include "diagnostics/incident.h"
#include "menu/assets.h"
#include "menu/render_interface.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <cstdio>
#include <RmlUi/Core.h>
#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/keybinds.h>

namespace narutorise::menu {
namespace {
// The UI's place among the presenter's drawers: above the SDK's ImGui.
constexpr size_t kPresenterZOrder = 2000;

// The key of RmlUi for a key of the window; unknown for the keys the menus do not use.
Rml::Input::KeyIdentifier Key(rex::ui::VirtualKey key) {
  using rex::ui::VirtualKey;
  using namespace Rml::Input;
  const auto in = [key](VirtualKey first, VirtualKey last) { return key >= first && key <= last; };
  const auto from = [key](VirtualKey first, KeyIdentifier base) {
    return KeyIdentifier(int(base) + int(key) - int(first));
  };
  if (in(VirtualKey::k0, VirtualKey::k9)) return from(VirtualKey::k0, KI_0);
  if (in(VirtualKey::kA, VirtualKey::kZ)) return from(VirtualKey::kA, KI_A);
  if (in(VirtualKey::kNumpad0, VirtualKey::kNumpad9)) return from(VirtualKey::kNumpad0, KI_NUMPAD0);
  if (in(VirtualKey::kF1, VirtualKey::kF12)) return from(VirtualKey::kF1, KI_F1);
  // Page Up, Page Down, End, Home, then Left, Up, Right, Down: the same order in both.
  if (in(VirtualKey::kPrior, VirtualKey::kDown)) return from(VirtualKey::kPrior, KI_PRIOR);
  switch (key) {
    case VirtualKey::kSpace: return KI_SPACE;
    case VirtualKey::kReturn: return KI_RETURN;
    case VirtualKey::kEscape: return KI_ESCAPE;
    case VirtualKey::kTab: return KI_TAB;
    case VirtualKey::kBack: return KI_BACK;
    case VirtualKey::kDelete: return KI_DELETE;
    case VirtualKey::kInsert: return KI_INSERT;
    default: return KI_UNKNOWN;
  }
}
int Modifiers(const rex::ui::KeyEvent& e) {
  return (e.is_ctrl_pressed() ? Rml::Input::KM_CTRL : 0) | (e.is_shift_pressed() ? Rml::Input::KM_SHIFT : 0) |
         (e.is_alt_pressed() ? Rml::Input::KM_ALT : 0);
}
int Button(rex::ui::MouseEvent::Button button) {
  switch (button) {
    case rex::ui::MouseEvent::Button::kLeft: return 0;
    case rex::ui::MouseEvent::Button::kRight: return 1;
    case rex::ui::MouseEvent::Button::kMiddle: return 2;
    default: return -1;
  }
}
}  // namespace

struct Ui::DialogEvents : Rml::EventListener {
  Ui* owner;
  std::vector<std::pair<std::string, std::function<void()>>> actions;
  bool pending = false;
  size_t selected = 0;
  void ProcessEvent(Rml::Event& event) override {
    if (event.GetType() == "click") {
      selected = size_t(event.GetCurrentElement()->GetAttribute<int>("action", 0));
      pending = true;
    } else if (event.GetType() == "keydown" && event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE) {
      selected = actions.empty() ? 0 : actions.size() - 1;
      pending = !actions.empty();
      event.StopPropagation();
    }
  }
};
struct Ui::MenuEvents : Rml::EventListener {
  std::unordered_map<std::string, std::function<void()>> actions;
  std::function<void()> pending;
  std::string last_focus = "overlay-settings";
  void ProcessEvent(Rml::Event& event) override {
    auto* target = event.GetTargetElement();
    while (target && target->GetTagName() != "button") target = target->GetParentNode();
    if (target) last_focus = target->GetId();
    if (event.GetType() == "keydown" && event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE) {
      pending = actions["close"]; event.StopPropagation();
    } else if (target && event.GetType() == "mouseover") target->Focus();
    else if (target && event.GetType() == "click") {
      if (auto it = actions.find(target->GetId()); it != actions.end()) pending = it->second;
    }
  }
};

Ui::Ui() = default;
Ui::~Ui() { Shutdown(); }

bool Ui::Initialize(rex::ui::ImmediateDrawer& drawer, rex::ui::Presenter* presenter) {
  if (initialized_) return true;
  files_ = std::make_unique<AssetFiles>();
  render_ = std::make_unique<RenderInterface>(drawer);
  presenter_ = presenter;
  Rml::SetFileInterface(files_.get());
  Rml::SetRenderInterface(render_.get());
  Rml::SetSystemInterface(&text_);
  if (!Rml::Initialise()) {
    REXLOG_ERROR("[narutorise] UI: RmlUi did not initialize");
    return false;
  }
  initialized_ = true;
  const struct {
    const char* path;
    Rml::Style::FontWeight weight;
    const char* family;
  } faces[] = {{"fonts/RobotoCondensed-Regular.ttf", Rml::Style::FontWeight::Normal, "Roboto Condensed"},
               {"fonts/RobotoCondensed-Bold.ttf", Rml::Style::FontWeight::Bold, "Roboto Condensed"},
               {"fonts/RobotoCondensed-Medium.ttf", Rml::Style::FontWeight(500), "Roboto Condensed"},
               {"fonts/Roboto-Regular.ttf", Rml::Style::FontWeight::Normal, "Roboto"}};
  for (const auto& face : faces) {
    const auto bytes = Asset(face.path);
    if (!Rml::LoadFontFace({bytes.data(), bytes.size()}, face.family, Rml::Style::FontStyle::Normal,
                           face.weight, face.weight == Rml::Style::FontWeight::Normal))
      REXLOG_WARN("[narutorise] UI: the font {} did not load", face.path);
  }
  context_ = Rml::CreateContext("game", {1, 1});
  fps_ = context_ ? context_->LoadDocument("fps.rml") : nullptr;
  status_ = context_ ? context_->LoadDocument("status.rml") : nullptr;
  if (status_) {
    status_events_ = std::make_unique<MenuEvents>();
    status_->AddEventListener("click", status_events_.get());
  }
  if (!fps_) REXLOG_WARN("[narutorise] UI: the frame rate document did not load");
  rate_time_ = std::chrono::steady_clock::now();
  pad_.Initialize();
  return context_ != nullptr;
}

void Ui::Shutdown() {
  if (!initialized_) return;
  if (presenter_ && in_presenter_) presenter_->RemoveUIDrawerFromUIThread(this);
  presenter_ = nullptr;
  in_presenter_ = false;
  HideOverlay();
  if (status_) status_->RemoveEventListener("click", status_events_.get());
  status_events_.reset(); status_ = nullptr;
  status_shown_ = false; game_menu_enabled_ = false; toggle_menu_ = {};
  CancelKeyCapture();
  HideDialog();
  panel_.Hide();
  controls_.Hide(); controls_shown_ = false;
  setup_.SetControlsOpen(false);
  panel_shown_ = false;
  pad_.Shutdown();
  controls::Hardware().Shutdown();
  tick_ = {};
  setup_.Hide();
  fps_visible_ = false;
  setup_shown_ = false;
  Rml::Shutdown();  // The documents, the context, its geometry and textures.
  initialized_ = false;
  context_ = nullptr;
  fps_ = nullptr;
  render_.reset();
  files_.reset();
  Rml::SetSystemInterface(nullptr);
}

void Ui::SetFpsVisible(bool visible) {
  if (!fps_ || visible == fps_visible_.load()) return;
  fps_visible_ = visible;
  UpdateDocuments();
}

bool Ui::ShowSetup(const SetupScreen::State& state, SetupScreen::Actions actions) {
  if (context_) context_->ProcessMouseMove(-1, -1, 0);
  if (!context_ || !setup_.Show(*context_, state, std::move(actions))) return false;
  setup_shown_ = true;
  setup_.SetGamepadHints(gamepad_input_);
  ++view_revision_;
  UpdateDocuments();
  return true;
}
void Ui::RefreshSetup(const SetupScreen::State& state) {
  if (context_) { context_->ProcessMouseMove(-1, -1, 0); setup_.Refresh(*context_, state); }
}

void Ui::HideSetup() {
  CancelKeyCapture();
  HideDialog();
  HidePanel();
  setup_.Hide();
  setup_shown_ = false;
  UpdateDocuments();
}
void Ui::CaptureKey(std::function<void(std::string)> captured) {
  key_capture_ = std::move(captured); pending_key_.clear(); capture_started_ = std::chrono::steady_clock::now();
}
void Ui::ShowPanel(std::string title, std::vector<std::string> tabs, int tab, std::vector<PanelRow> rows,
                    std::function<void(int)> select_tab, std::function<void()> close, std::string focus) {
  if (!context_) return;
  controls_.Hide(); controls_shown_ = false;
  ++view_revision_;
  setup_.SetControlsOpen(false);
  context_->ProcessMouseMove(-1, -1, 0);
  panel_shown_ = panel_.Show(*context_, std::move(title), std::move(tabs), tab, std::move(rows),
                              std::move(select_tab), std::move(close), std::move(focus), setup_.Shown());
  panel_.SetGamepadHints(gamepad_input_);
  UpdateDocuments();
}
void Ui::HidePanel() { ++view_revision_; controls_.Hide(); controls_shown_ = false; setup_.SetControlsOpen(false); panel_.Hide(); panel_shown_ = false; UpdateDocuments(); }
void Ui::ShowControls(controls::Config config,std::function<bool(const controls::Config&)> save,std::function<void()> close,std::string menu_key) {
  if (!context_) return;
  ++view_revision_;
  controls::Hardware().StopAllRumble();
  CancelKeyCapture(); panel_.Hide(); panel_shown_ = false;
  controls_shown_ = controls_.Show(*context_, std::move(config), std::move(save), std::move(close), std::move(menu_key), setup_.Shown());
  setup_.SetControlsOpen(controls_shown_.load());
  controls_.SetGamepadHints(gamepad_input_); UpdateDocuments();
}

void Ui::EnableGameMenu(std::string key, std::string chord, std::function<void()> toggle) {
  menu_key_ = std::move(key); toggle_menu_ = std::move(toggle); game_menu_enabled_ = bool(toggle_menu_);
  pad_.SetShortcut(std::move(chord), [this] { if ((!input_active_ || input_active_()) && toggle_menu_) toggle_menu_(); });
}
void Ui::ShowOverlay(std::function<void()> settings, std::function<void()> dlc,
                      std::function<void()> quit, std::function<void()> close) {
  controls::Hardware().StopAllRumble();
  HideOverlay();
  if (!context_) return;
  context_->ProcessMouseMove(-1, -1, 0);
  overlay_ = context_->LoadDocument("overlay.rml");
  if (!overlay_) return;
  overlay_events_ = std::make_unique<MenuEvents>();
  overlay_events_->actions = {{"overlay-settings", std::move(settings)}, {"overlay-dlc", std::move(dlc)},
                             {"overlay-exit", std::move(quit)}, {"close", close}, {"overlay-close", std::move(close)}};
  for (const auto* id : {"overlay-settings", "overlay-dlc", "overlay-exit"}) {
    auto* button = overlay_->GetElementById(id);
    const std::string label = Text(std::string(id) == "overlay-settings" ? "settings" : std::string(id) == "overlay-dlc" ? "dlc" : "exit-game");
    button->SetInnerRML("<div class='tab-plate'/><span class='tab-label'>" + Rml::StringUtilities::EncodeRml(label) + "</span>");
  }
  overlay_->GetElementById("overlay-close-hint")->SetInnerRML(Text(gamepad_input_ ? "close-pad" : "close-key"));
  for (const char* event : {"click", "keydown", "mouseover"}) overlay_->AddEventListener(event, overlay_events_.get());
  overlay_->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
  overlay_->GetElementById("overlay-settings")->Focus();
  overlay_shown_ = true; UpdateDocuments();
  ++view_revision_;
}
void Ui::HideOverlay() {
  if (!overlay_) return;
  HideDialog(); HidePanel(); CancelKeyCapture();
  for (const char* event : {"click", "keydown", "mouseover"}) overlay_->RemoveEventListener(event, overlay_events_.get());
  overlay_->Close(); overlay_ = nullptr; overlay_events_.reset();
  overlay_shown_ = false; UpdateDocuments();
}
void Ui::UseGamepad(bool gamepad) {
  if (gamepad_input_ == gamepad) return;
  gamepad_input_ = gamepad; panel_.SetGamepadHints(gamepad);
  controls_.SetGamepadHints(gamepad);
  setup_.SetGamepadHints(gamepad);
  if (overlay_) overlay_->GetElementById("overlay-close-hint")->SetInnerRML(Text(gamepad ? "close-pad" : "close-key"));
}
void Ui::Notify(std::string message, std::string action_label, std::function<void()> action, std::string title) {
  notices_.push_back({std::move(message), std::move(action_label), std::move(action), std::move(title)}); UpdateStatus();
}
void Ui::ShowShortcutHint(std::string message, std::chrono::milliseconds delay) {
  if (!status_) return;
  status_->GetElementById("hint-text")->SetInnerRML(Rml::StringUtilities::EncodeRml(message));
  hint_start_ = std::chrono::steady_clock::now() + delay;
  hint_end_ = hint_start_ + std::chrono::milliseconds(2500);
  hint_pending_ = true; UpdateStatus();
}
void Ui::SetShadersVisible(bool visible, uint32_t pending, float progress) {
  if (shaders_visible_ == visible && shader_pending_ == pending && shader_progress_ == progress) return;
  shaders_visible_ = visible; shader_pending_ = pending; shader_progress_ = progress; UpdateStatus();
}
void Ui::UpdateStatus() {
  if (!status_) return;
  const auto now = std::chrono::steady_clock::now();
  hint_shown_ = hint_pending_ && now >= hint_start_ && now < hint_end_ && !overlay_shown_.load();
  if (now >= hint_end_) hint_pending_ = false;
  status_->SetClass("hint", hint_shown_);
  if (notice_active_ && now >= notice_until_) notice_active_ = false;
  if (!notice_active_ && !notices_.empty()) {
    auto next = std::move(notices_.front()); notices_.pop_front();
    status_->GetElementById("notice-title")->SetInnerRML(Rml::StringUtilities::EncodeRml(next.title.empty() ? next.text : next.title));
    status_->GetElementById("notice-text")->SetInnerRML(Rml::StringUtilities::EncodeRml(next.title.empty() ? next.action_label : next.text));
    status_->SetClass("action", bool(next.action));
    status_events_->actions["notice-action"] = std::move(next.action);
    notice_active_ = true; notice_until_ = now + std::chrono::seconds(7);
  }
  status_->SetClass("notice", notice_active_);
  status_->SetClass("compiling", shaders_visible_);
  char count_text[128]; std::snprintf(count_text, sizeof(count_text), Text("shader-count"), shader_pending_);
  const std::string shader_text = shader_pending_ ? count_text : Text("shaders");
  if (shaders_text_ != shader_text) {
    shaders_text_ = shader_text; status_->GetElementById("shader-text")->SetInnerRML(shaders_text_);
  }
  if (shaders_visible_) {
    const float fraction = shader_progress_ < 0 ? .35f : std::clamp(shader_progress_, 0.0f, 1.0f);
    status_->GetElementById("shader-progress")->SetProperty("width", fmt::format("{:.1f}%", fraction * 100));
  }
  status_shown_ = notice_active_ || shaders_visible_ || hint_shown_;
  if (status_shown_.load() && !status_->IsVisible()) status_->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
  else if (!status_shown_.load() && status_->IsVisible()) status_->Hide();
}
void Ui::PollInput() {
  incident::Activity activity(incident::Role::kUi);
  incident::Progress(incident::Role::kUi);
  if (!initialized_) return;
  if (tick_) tick_();
  const bool focused = !input_active_ || input_active_();
  controls::Hardware().Poll(controls::Backend(), focused);
  controls_.Tick(focused);
  UpdateStatus();
  if (controls_.BlocksNavigation()) return;
  pad_.PollDevices(*controls::Hardware().Devices(), TakesInput() && focused, [this](rex::ui::VirtualKey key) {
    UseGamepad(true); dispatching_gamepad_ = true;
    rex::ui::KeyEvent down(nullptr, key, 1, false, false, false, false, false);
    rex::ui::KeyEvent up(nullptr, key, 1, true, false, false, false, false);
    OnKeyDown(down); OnKeyUp(up);
    dispatching_gamepad_ = false;
  }, controls_.UseStickNavigation());
}

void Ui::HideDialog() {
  if (dialog_) {
    dialog_->RemoveEventListener("keydown", dialog_events_.get());
    if (auto* buttons = dialog_->GetElementById("dialog-actions")) {
      for (int i = 0; i < buttons->GetNumChildren(); ++i)
        buttons->GetChild(i)->RemoveEventListener("click", dialog_events_.get());
    }
    dialog_->Close();
  }
  dialog_ = nullptr;
  dialog_shown_ = false;
  dialog_events_.reset();
  UpdateDocuments();
}
void Ui::ShowDialog(const std::string& title, const std::string& message,
                    std::vector<std::pair<std::string, std::function<void()>>> actions) {
  HideDialog();
  if (!context_) return;
  dialog_ = context_->LoadDocument("dialog.rml");
  if (!dialog_) return;
  dialog_->GetElementById("dialog-title")->SetInnerRML(Rml::StringUtilities::EncodeRml(title));
  dialog_->GetElementById("dialog-message")->SetInnerRML(Rml::StringUtilities::EncodeRml(message));
  dialog_events_ = std::make_unique<DialogEvents>();
  dialog_events_->owner = this;
  dialog_events_->actions = std::move(actions);
  auto* buttons = dialog_->GetElementById("dialog-actions");
  for (size_t i = dialog_events_->actions.size(); i-- > 0;) {
    auto element = dialog_->CreateElement("button");
    element->SetAttribute("action", int(i));
    element->SetInnerRML("<div class='button-plate'/><span class='button-label'>" + Rml::StringUtilities::EncodeRml(dialog_events_->actions[i].first) + "</span>");
    element->SetProperty("width", fmt::format("{}dp", std::clamp(int(dialog_events_->actions[i].first.size()) * 8 + 40, 140, 240)));
    element->AddEventListener("click", dialog_events_.get());
    buttons->AppendChild(std::move(element));
  }
  dialog_->AddEventListener("keydown", dialog_events_.get());
  dialog_->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Document);
  if (buttons->GetNumChildren()) buttons->GetChild(0)->Focus();
  dialog_shown_ = true;
  UpdateDocuments();
}

// The frame rate shows unless the setup screen does; the presenter draws the
// Ui while something shows.
void Ui::UpdateDocuments() {
  UpdateStatus();
  if (overlay_) {
    const bool bar = !panel_.Shown() && !controls_.Shown() && !dialog_;
    if (bar && !overlay_->IsVisible()) {
      overlay_->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
      if (auto* item = overlay_->GetElementById(overlay_events_->last_focus)) item->Focus();
    } else if (!bar && overlay_->IsVisible()) overlay_->Hide();
  }
  if (fps_) fps_->SetClass("menu-open", overlay_shown_.load());
  if (fps_) {
    const bool fps = fps_visible_.load() && !setup_shown_.load() && !panel_.Shown() && !controls_.Shown() && !dialog_;
    if (fps && !fps_->IsVisible()) {
      rate_frames_ = frames_.load(std::memory_order_relaxed);
      rate_time_ = std::chrono::steady_clock::now();
      fps_->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    } else if (!fps && fps_->IsVisible()) {
      fps_->Hide();
    }
  }
  if (presenter_ && Shown() != in_presenter_) {
    in_presenter_ = Shown();
    if (in_presenter_) presenter_->AddUIDrawerFromUIThread(this, kPresenterZOrder);
    else presenter_->RemoveUIDrawerFromUIThread(this);
  }
}

void Ui::UpdateRate() {
  const auto now = std::chrono::steady_clock::now();
  if (now - rate_time_ < kRatePeriod) return;
  double fps = 0, ms = 0;
  if (!rate_source_ || !rate_source_(fps, ms)) {
    const uint64_t frames = frames_.load(std::memory_order_relaxed);
    const double seconds = std::chrono::duration<double>(now - rate_time_).count();
    const uint64_t counted = frames - rate_frames_;
    rate_frames_ = frames;
    fps = counted / seconds;
    ms = counted ? seconds * 1000.0 / counted : 0;
  }
  rate_time_ = now;
  if (!fps_) return;
  if (Rml::Element* rate = fps_->GetElementById("rate")) {
    rate->SetInnerRML(fmt::format("{:.0f} FPS", fps));
    rate->SetClass("good", fps >= 55);
    rate->SetClass("fair", fps >= 30 && fps < 55);
    rate->SetClass("poor", fps < 30);
  }
  if (Rml::Element* time = fps_->GetElementById("time")) {
    auto value = fmt::format("{:.1f} ms", ms);
    if (Localization::GetLanguage() != Localization::Language::English) std::replace(value.begin(), value.end(), '.', ',');
    time->SetInnerRML(value);
  }
}

void Ui::Draw(rex::ui::UIDrawContext& context) {
  if (!context_) return;
  if (dialog_events_ && dialog_events_->pending) {
    auto action = dialog_events_->actions.at(dialog_events_->selected).second;
    HideDialog();
    if (action) action();
  }
  if (overlay_events_ && overlay_events_->pending) {
    auto action = std::exchange(overlay_events_->pending, {}); if (action) action();
  }
  if (status_events_ && status_events_->pending) {
    auto action = std::exchange(status_events_->pending, {}); if (action) action();
  }
  if (key_capture_ && (!pending_key_.empty() || std::chrono::steady_clock::now() - capture_started_ > std::chrono::seconds(5))) {
    auto captured = std::exchange(key_capture_, {});
    captured(std::exchange(pending_key_, {}));
  }
  panel_.Tick();
  PollInput();
  const int width = int(context.render_target_width()), height = int(context.render_target_height());
  if (width <= 0 || height <= 0) return;
  if (width != width_ || height != height_) {
    width_ = width;
    height_ = height;
    context_->SetDimensions({width, height});
    context_->SetDensityIndependentPixelRatio(std::max(1.0f, float(height) / 720.0f));
    panel_.Layout(width, height, context_->GetDensityIndependentPixelRatio());
    controls_.Layout(width, height, context_->GetDensityIndependentPixelRatio());
  }
  if (fps_ && fps_->IsVisible()) UpdateRate();
  setup_.UpdateLogo(*render_);
  context_->Update();
  if (panel_.RevealSelection()) context_->Update();
  render_->Begin(context, width, height);
  context_->Render();
  render_->End();
  // The presenter paints the UI again only when asked.
  if (presenter_ && Shown()) presenter_->RequestUIPaintFromUIThread();
}

void Ui::OnKeyDown(rex::ui::KeyEvent& e) {
  if (controls_.Shown() && !dispatching_gamepad_) controls_.Key(e.virtual_key(), true, e.prev_state());
  if (controls_.BlocksNavigation()) { e.set_handled(true); return; }
  if (!dispatching_gamepad_) UseGamepad(false);
  if (key_capture_) {
    e.set_handled(true);
    if (!e.prev_state()) {
      if (e.virtual_key() == rex::ui::VirtualKey::kEscape) {
        pending_key_.clear(); capture_started_ = {}; // Cancel on the next frame.
      } else pending_key_ = rex::ui::VirtualKeyToString(e.virtual_key());
    }
    return;
  }
  if (toggle_menu_ && !e.is_alt_pressed() && e.virtual_key() == rex::ui::ParseVirtualKey(menu_key_)) {
    e.set_handled(true); if (!e.prev_state()) toggle_menu_(); return;
  }
  if (!TakesInput() || e.is_alt_pressed()) return;
  e.set_handled(true);
  const auto key = Key(e.virtual_key());
  if (key != Rml::Input::KI_UNKNOWN) context_->ProcessKeyDown(key, Modifiers(e));
}

void Ui::OnKeyUp(rex::ui::KeyEvent& e) {
  if (controls_.Shown() && !dispatching_gamepad_) controls_.Key(e.virtual_key(), false);
  if (controls_.BlocksNavigation()) { e.set_handled(!overlay_shown_.load()); return; }
  if (toggle_menu_ && e.virtual_key() == rex::ui::ParseVirtualKey(menu_key_)) return;
  if (key_capture_) { e.set_handled(!overlay_shown_.load()); return; }
  if (!TakesInput() || e.is_alt_pressed()) return;
  // Releases reach the SDK to clear keys held before opening. The driver gate
  // still returns neutral input, and UI key-downs never reach the SDK.
  e.set_handled(!overlay_shown_.load());
  const auto key = Key(e.virtual_key());
  if (key != Rml::Input::KI_UNKNOWN) context_->ProcessKeyUp(key, Modifiers(e));
}

void Ui::OnMouseDown(rex::ui::MouseEvent& e) {
  if (controls_.Shown()) controls_.MouseButton(int(e.button()), true);
  if (controls_.BlocksNavigation()) { e.set_handled(true); return; }
  if (key_capture_) {
    context_->ProcessMouseMove(e.x(), e.y(), 0);
    auto* target = context_->GetHoverElement();
    while (target && !target->HasAttribute("action")) target = target->GetParentNode();
    if (target) {
      CancelKeyCapture(); // The modal's Cancel button is not a mouse binding.
    } else {
    e.set_handled(true);
    pending_key_ = e.button() == rex::ui::MouseEvent::Button::kLeft ? "LMB" :
                   e.button() == rex::ui::MouseEvent::Button::kRight ? "RMB" : "MMB";
    return;
    }
  }
  if (!TakesInput()) {
    if (!status_shown_.load()) return;
    context_->ProcessMouseMove(e.x(), e.y(), 0);
    auto* hovered = context_->GetHoverElement();
    if (!hovered || hovered->GetId() != "notice-action") return;
    notice_pressed_ = true;
  }
  e.set_handled(true);
  context_->ProcessMouseMove(e.x(), e.y(), 0);
  if (const int button = Button(e.button()); button >= 0) context_->ProcessMouseButtonDown(button, 0);
}

void Ui::OnMouseMove(rex::ui::MouseEvent& e) {
  if (controls_.Shown()) controls_.MouseMove(e.x(), e.y(), e.dx(), e.dy());
  if (controls_.BlocksNavigation()) { e.set_handled(true); return; }
  UseGamepad(false);
  if (!TakesInput() && !status_shown_.load()) return;
  if (TakesInput()) e.set_handled(true);
  context_->ProcessMouseMove(e.x(), e.y(), 0);
}

void Ui::OnMouseUp(rex::ui::MouseEvent& e) {
  if (controls_.Shown()) controls_.MouseButton(int(e.button()), false);
  if (controls_.BlocksNavigation()) { e.set_handled(!overlay_shown_.load()); return; }
  if (!TakesInput() && !notice_pressed_) return;
  notice_pressed_ = false;
  e.set_handled(!overlay_shown_.load());
  if (const int button = Button(e.button()); button >= 0) context_->ProcessMouseButtonUp(button, 0);
}

void Ui::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (controls_.BlocksNavigation()) { e.set_handled(true); return; }
  if (!TakesInput()) return;
  e.set_handled(true);
  context_->ProcessMouseWheel(-float(e.scroll_y()) / float(rex::ui::MouseEvent::kScrollPerDetent), 0);
}

}  // namespace narutorise::menu
