#include "ui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <mutex>

#include "content_manager.h"
#include "game_paths.h"
#include "localization.h"
#include "theme.h"

namespace {

int g_active_tab = 0;
std::mutex g_dialog_mutex;
std::string g_selected_folder;
bool g_dialog_pending = false;
bool g_dialog_updated = false;

// DLC folder selection state
std::mutex g_dlc_dialog_mutex;
std::string g_dlc_selected_folder;
bool g_dlc_dialog_pending = false;
bool g_dlc_dialog_updated = false;



const ImVec4 kAccent(1.0f, 0.49f, 0.16f, 1.0f);
const ImVec4 kGreen(0.48f, 0.84f, 0.64f, 1.0f);
const ImVec4 kWarning(1.0f, 0.68f, 0.38f, 1.0f);

std::string PathText(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

void SDLCALL FolderDialogCallback(void*, const char* const* files, int) {
  std::lock_guard lock(g_dialog_mutex);
  if (files && files[0]) {
    g_selected_folder = files[0];
    g_dialog_updated = true;
  }
  g_dialog_pending = false;
}

void SDLCALL DlcFolderDialogCallback(void*, const char* const* files, int) {
  std::lock_guard lock(g_dlc_dialog_mutex);
  if (files && files[0]) {
    g_dlc_selected_folder = files[0];
    g_dlc_dialog_updated = true;
  }
  g_dlc_dialog_pending = false;
}

void Heading(const char* text, float size = 28.0f) {
  auto& fonts = ImGui::GetIO().Fonts->Fonts;
  ImGui::PushFont(fonts.Size > 1 ? fonts[1] : ImGui::GetIO().FontDefault, size);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
}

void Muted(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

bool PrimaryButton(const char* label, ImVec2 size) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.91f, 0.36f, 0.07f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.47f, 0.12f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.76f, 0.28f, 0.04f, 1));
  const bool clicked = ImGui::Button(label, size);
  ImGui::PopStyleColor(3);
  return clicked;
}

void BeginCard(const char* id, float height) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 16));
  ImGuiChildFlags flags = ImGuiChildFlags_Borders;
  if (height == 0) flags |= ImGuiChildFlags_AutoResizeY;
  ImGui::BeginChild(id, ImVec2(0, height), flags,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}

void EndCard() {
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

std::string DisplayName() {
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
  if (!mode || mode->h <= 0) return "—";
  const double aspect = static_cast<double>(mode->w) / mode->h;
  const char* ratio = aspect >= 3.4 ? "32:9" : aspect >= 2.2 ? "21:9" :
                      aspect >= 1.7 ? "16:9" : aspect >= 1.55 ? "16:10" : "4:3";
  char buffer[80];
  std::snprintf(buffer, sizeof(buffer), "%d × %d  ·  %s", mode->w, mode->h, ratio);
  return buffer;
}

std::string ControllerName() {
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  std::string result = Tr(TextId::NoControllerDetected);
  if (ids && count > 0) {
    const char* name = SDL_GetGamepadNameForID(ids[0]);
    result = name ? name : "Gamepad";
  }
  SDL_free(ids);
  return result;
}

void SettingLabel(TextId id) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(Tr(id));
  ImGui::TableNextColumn();
  ImGui::SetNextItemWidth(-1);
}

void ApplyRecommended(GameConfig& config) {
  config.resolution_scale = config.draw_resolution_scale_x = config.draw_resolution_scale_y = 2;
  config.vsync = config.d3d12_host_vsync = config.fullscreen = config.skip_intro_videos = true;
  config.anisotropic_override = 5;
  config.swap_post_effect = "fxaa";
  config.present_effect = "cas";
  config.present_dither = false;
  config.show_fps_overlay = false;
  config.input_backend = "sdl";
  config.ultrawide_target_aspect = 0;
  const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
  if (mode && mode->h > 0) {
    const double aspect = static_cast<double>(mode->w) / mode->h;
    if (aspect >= 3.4) config.ultrawide_target_aspect = 3.5556;
    else if (aspect >= 2.2) config.ultrawide_target_aspect = 2.3889;
  }
}

void OpenFolder(const std::filesystem::path& folder) {
  const auto path = folder.generic_u8string();
  // Encode reserved URI bytes so paths with spaces, accents or '#' open correctly.
  std::string url = "file:///";
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : path) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '/' || c == ':' || c == '-' || c == '_' || c == '.') {
      url += static_cast<char>(c);
    } else {
      url += '%'; url += hex[c >> 4]; url += hex[c & 15];
    }
  }
  SDL_OpenURL(url.c_str());
}

} // namespace

void LauncherUI::SetupTheme() { launcher_theme::Apply(); }

bool LauncherUI::Render(SDL_Window* window, const std::filesystem::path& base_dir,
                        GameConfig& config, bool& request_launch, std::string& status_message,
                        ID3D11ShaderResourceView* cover_texture, int cover_width, int cover_height) {
  Localization::SetLanguageCode(config.launcher_language);
  bool dialog_pending;
  {
    std::lock_guard lock(g_dialog_mutex);
    if (g_dialog_updated) {
      const auto selected = std::filesystem::u8path(g_selected_folder);
      std::error_code folder_error;
      if (std::filesystem::is_regular_file(selected / "default.xex", folder_error)) {
        config.custom_game_root = g_selected_folder;
        status_message = Tr(TextId::PathUpdatedMsg) + g_selected_folder;
      } else {
        status_message = Tr(TextId::FolderHint);
      }
      g_dialog_updated = false;
    }
    dialog_pending = g_dialog_pending;
  }

  // Process DLC folder selection dialog result.
  {
    std::lock_guard lock(g_dlc_dialog_mutex);
    if (g_dlc_dialog_updated) {
      const auto selected = std::filesystem::u8path(g_dlc_selected_folder);
      std::error_code ec;
      if (std::filesystem::is_directory(selected, ec)) {
        std::string error;
        // STFS packages are staged into the game's dlc/ folder and
        // installed by the game on the next launch (real content headers
        // with the license from the package); loose content is copied
        // directly into the user data tree.
        const int staged = launcher_content::InstallDlc(selected, base_dir, error);
        if (staged > 0) {
          status_message = Tr(TextId::DlcStfsStaged);
        } else if (staged == 0) {
          status_message = Tr(TextId::DlcInstallSuccess);
        } else {
          status_message = std::string(Tr(TextId::DlcInstallFailed)) + " " + error;
        }
      }
      g_dlc_dialog_updated = false;
    }
  }



  const auto game_root = game_paths::Find(base_dir, config.custom_game_root);
  const bool files_found = !game_root.empty();
#if defined(_WIN32)
  const auto executable = base_dir / "narutorise.exe";
#else
  const auto executable = base_dir / "narutorise";
#endif
  std::error_code ec;
  const bool exe_found = std::filesystem::is_regular_file(executable, ec);
  const bool ready = files_found && exe_found;
  const char* state = !files_found ? Tr(TextId::StatusNotFound) :
                      !exe_found ? Tr(TextId::ExeMissingStatus) : Tr(TextId::StatusReady);

  // ImGui's display size is in logical coordinates, including on high-DPI displays.
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float cover_w = std::clamp(size.x * 0.35f, 330.0f, 440.0f);
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(size);
  ImGui::Begin("LauncherRoot", nullptr, ImGuiWindowFlags_NoDecoration |
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

  auto* draw = ImGui::GetWindowDrawList();
  if (cover_texture && cover_width > 0 && cover_height > 0) {
    // Cover fit: crop symmetrically instead of stretching the artwork.
    const float image_aspect = static_cast<float>(cover_width) / cover_height;
    const float panel_aspect = cover_w / size.y;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (panel_aspect < image_aspect) {
      const float visible = panel_aspect / image_aspect;
      uv0.x = (1 - visible) / 2; uv1.x = 1 - uv0.x;
    } else {
      const float visible = image_aspect / panel_aspect;
      uv0.y = (1 - visible) / 2; uv1.y = 1 - uv0.y;
    }
    draw->AddImage(reinterpret_cast<ImTextureID>(cover_texture), ImVec2(0, 0),
                   ImVec2(cover_w, size.y), uv0, uv1);
  } else {
    draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(cover_w, size.y),
        IM_COL32(59, 37, 27, 255), IM_COL32(31, 29, 35, 255),
        IM_COL32(17, 18, 23, 255), IM_COL32(27, 22, 22, 255));
    ImGui::SetCursorPos(ImVec2(28, size.y * 0.35f));
    Heading("NARUTO", 38);
    ImGui::TextUnformatted("RISE OF A NINJA");
  }
  draw->AddRectFilledMultiColor(ImVec2(0, size.y - 100), ImVec2(cover_w, size.y),
      IM_COL32(8, 9, 13, 0), IM_COL32(8, 9, 13, 0),
      IM_COL32(8, 9, 13, 230), IM_COL32(8, 9, 13, 230));
  draw->AddLine(ImVec2(cover_w, 0), ImVec2(cover_w, size.y), IM_COL32(122, 64, 31, 255));
  ImGui::SetCursorPos(ImVec2(24, size.y - 40));
  ImGui::TextUnformatted("PC EDITION  /  ReXGlue");

  ImGui::SetCursorPos(ImVec2(cover_w, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));
  ImGui::BeginChild("ContentPanel", ImVec2(size.x - cover_w, size.y),
                    ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  const float available_w = ImGui::GetContentRegionAvail().x;
  const TextId tabs[] = {TextId::TabPlay, TextId::TabSettings, TextId::TabGameFiles, TextId::TabDlc};
  const float lang_w = 130;
  float tab_widths[4];
  float remaining_tab_space = available_w - lang_w - 28;
  for (int i = 0; i < 4; ++i) {
    tab_widths[i] = ImGui::CalcTextSize(Tr(tabs[i])).x + ImGui::GetStyle().FramePadding.x * 2;
    remaining_tab_space -= tab_widths[i];
  }
  for (int i = 0; i < 4; ++i) {
    if (i) ImGui::SameLine(0, 4);
    ImGui::PushID(i);
    ImGui::PushStyleColor(ImGuiCol_Button, g_active_tab == i ? ImVec4(0.91f, 0.36f, 0.07f, 1) : ImVec4(0.13f, 0.13f, 0.16f, 1));
    const float tab_w = tab_widths[i] + std::max(0.0f, remaining_tab_space / 4);
    if (ImGui::Button(Tr(tabs[i]), ImVec2(tab_w, 38))) g_active_tab = i;
    ImGui::PopStyleColor();
    ImGui::PopID();
  }
  ImGui::SameLine(0, 16);
  ImGui::SetNextItemWidth(lang_w);
  const auto& languages = Localization::GetLanguages();
  if (ImGui::BeginCombo("##Language", languages[static_cast<size_t>(Localization::GetLanguage())].name)) {
    for (const auto& option : languages) {
      const bool selected = Localization::GetLanguage() == option.language;
      if (ImGui::Selectable(option.name, selected)) {
        config.launcher_language = option.code;
        Localization::SetLanguage(option.language);
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr(TextId::LauncherLanguageTooltip));
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // Reserve a fixed footer and (on Settings) an action bar outside the scroll area.
  const float actions_h = g_active_tab == 1 ? 94.0f : 0;
  const float body_h = ImGui::GetContentRegionAvail().y - 54 - actions_h;
  ImGui::PushID(g_active_tab);
  ImGui::BeginChild("TabBody", ImVec2(0, body_h));

  if (g_active_tab == 0) {
    ImGui::Spacing();
    Heading(Tr(TextId::PlayHeading));
    Muted(Tr(TextId::PlayIntro));
    ImGui::Dummy(ImVec2(0, 24));
    BeginCard("ReadyCard", 130);
    const ImVec2 dot = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(dot.x + 6, dot.y + 13), 5,
        ImGui::ColorConvertFloat4ToU32(ready ? kGreen : kWarning));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 22);
    ImGui::PushStyleColor(ImGuiCol_Text, ready ? kGreen : kWarning);
    Heading(state, 21);
    ImGui::PopStyleColor();
    ImGui::Spacing();
    Muted(ready ? Tr(TextId::ReadyDetail) : !files_found ? Tr(TextId::FilesMissingDesc) : Tr(TextId::ExeMissing));
    EndCard();

    // A single obvious primary action, centered like the reference launcher.
    ImGui::Dummy(ImVec2(0, std::max(20.0f, body_h * 0.10f)));
    const float play_w = std::min(300.0f, available_w - 40);
    ImGui::SetCursorPosX((available_w - play_w) / 2);
    ImGui::BeginDisabled(!ready);
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.Size > 1 ? ImGui::GetIO().Fonts->Fonts[1] : ImGui::GetIO().FontDefault, 27);
    if (PrimaryButton(Tr(TextId::BtnPlay), ImVec2(play_w, 74))) request_launch = true;
    ImGui::PopFont();
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::SetCursorPosX((available_w - play_w) / 2);
    if (ImGui::Button(Tr(TextId::ManageFiles), ImVec2(play_w, 36))) g_active_tab = 2;
    ImGui::Spacing();
    ImGui::SetCursorPosX((available_w - play_w) / 2);
    if (ImGui::Button(Tr(TextId::Exit), ImVec2(play_w, 32))) {
      SDL_Event quit{}; quit.type = SDL_EVENT_QUIT; SDL_PushEvent(&quit);
    }
  } else if (g_active_tab == 1) {
    Heading(Tr(TextId::TabSettings));
    Muted(Tr(TextId::SettingsIntro));
    ImGui::Dummy(ImVec2(0, 8));
    BeginCard("Game", 0);
    ImGui::TextColored(kAccent, "%s", Tr(TextId::GroupGame));
    ImGui::Spacing();
    if (ImGui::BeginTable("GameFields", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 160);
      ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
      SettingLabel(TextId::GameLanguageLabel);
      const struct {
        std::uint32_t code;
        TextId label;
      } game_languages[] = {
          {1, TextId::GameLanguageEnglish},
          {4, TextId::GameLanguageFrench},
          {3, TextId::GameLanguageGerman},
          {5, TextId::GameLanguageSpanish},
          {6, TextId::GameLanguageItalian},
      };
      // Preserve manually configured SDK languages until the user selects one.
      char custom_language[64];
      std::snprintf(custom_language, sizeof(custom_language), Tr(TextId::GameLanguageCustom),
                    static_cast<unsigned int>(config.user_language));
      const char* preview = custom_language;
      for (const auto& option : game_languages) {
        if (config.user_language == option.code) preview = Tr(option.label);
      }
      if (ImGui::BeginCombo("##GameLanguage", preview)) {
        for (const auto& option : game_languages) {
          const bool selected = config.user_language == option.code;
          if (ImGui::Selectable(Tr(option.label), selected)) config.user_language = option.code;
          if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::EndTable();
    }
    Muted(Tr(TextId::GameLanguageHint));
    ImGui::Spacing();
    ImGui::Checkbox(Tr(TextId::SkipIntroLabel), &config.skip_intro_videos);
    EndCard();
    ImGui::Dummy(ImVec2(0, 4));
    BeginCard("Graphics", 438);
    ImGui::TextColored(kAccent, "%s", Tr(TextId::GroupGraphics));
    ImGui::Spacing();
    if (ImGui::BeginTable("GraphicsFields", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 160);
      ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
      SettingLabel(TextId::DisplayLabel);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(DisplayName().c_str());
      SettingLabel(TextId::AspectLabel);
      const char* aspects[] = {Tr(TextId::Aspect16x9), Tr(TextId::Aspect21x9), Tr(TextId::Aspect32x9)};
      int aspect = config.ultrawide_target_aspect >= 3.5 ? 2 : config.ultrawide_target_aspect >= 2.3 ? 1 : 0;
      if (ImGui::Combo("##Aspect", &aspect, aspects, 3)) config.ultrawide_target_aspect = aspect == 2 ? 3.5556 : aspect == 1 ? 2.3889 : 0;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr(TextId::AspectTooltip));
      SettingLabel(TextId::ResolutionLabel);
      const char* scales[] = {Tr(TextId::Res1x), Tr(TextId::Res2x), Tr(TextId::Res3x), Tr(TextId::Res4x)};
      int scale = std::clamp(config.resolution_scale - 1, 0, 3);
      if (ImGui::Combo("##Resolution", &scale, scales, 4)) {
        config.resolution_scale = config.draw_resolution_scale_x = config.draw_resolution_scale_y = scale + 1;
      }
      SettingLabel(TextId::RendererLabel);
      ImGui::AlignTextToFramePadding();
      Muted("ReXGlue · Direct3D 12");
      SettingLabel(TextId::TextureFilteringLabel);
      const char* filters[] = {Tr(TextId::AnisoDefault), "2x", "4x", "8x", "16x"};
      int filter = std::clamp(config.anisotropic_override - 1, 0, 4);
      if (ImGui::Combo("##Filter", &filter, filters, 5)) config.anisotropic_override = filter + 1;
      SettingLabel(TextId::AntiAliasingLabel);
      const char* aa_modes[] = {Tr(TextId::AaOff), Tr(TextId::AaFxaa), Tr(TextId::AaFxaaExtreme)};
      int aa = 0;
      if (config.swap_post_effect == "fxaa") aa = 1;
      else if (config.swap_post_effect == "fxaa_extreme") aa = 2;
      if (ImGui::Combo("##AA", &aa, aa_modes, 3)) {
        config.swap_post_effect = aa == 1 ? "fxaa" : aa == 2 ? "fxaa_extreme" : "none";
      }
      SettingLabel(TextId::PostProcessingLabel);
      const char* post_effects[] = {Tr(TextId::PostBilinear), Tr(TextId::PostCas), Tr(TextId::PostFsr)};
      int post = 0;
      if (config.present_effect == "cas" || config.present_effect == "cas_sharpen") post = 1;
      else if (config.present_effect == "fsr" || config.present_effect == "fsr_easu") post = 2;
      if (ImGui::Combo("##PostFX", &post, post_effects, 3)) {
        config.present_effect = post == 1 ? "cas" : post == 2 ? "fsr" : "bilinear";
      }
      ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::Checkbox(Tr(TextId::FullscreenLabel), &config.fullscreen);
    ImGui::SameLine(0, 24);
    if (ImGui::Checkbox(Tr(TextId::VsyncLabel), &config.vsync)) {
      config.d3d12_host_vsync = config.vsync;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr(TextId::VsyncTooltip));
    ImGui::SameLine(0, 24);
    ImGui::Checkbox(Tr(TextId::DitherLabel), &config.present_dither);
    ImGui::SameLine(0, 24);
    ImGui::Checkbox(Tr(TextId::FpsOverlayLabel), &config.show_fps_overlay);
    EndCard();
    ImGui::Dummy(ImVec2(0, 4));
    BeginCard("Controls", 126);
    ImGui::TextColored(kAccent, "%s", Tr(TextId::GroupControls));
    ImGui::Spacing();
    bool sdl = config.input_backend != "xinput";
    if (ImGui::Checkbox(Tr(TextId::SdlInputLabel), &sdl)) config.input_backend = sdl ? "sdl" : "xinput";
    Muted(ControllerName().c_str());
    EndCard();
  } else if (g_active_tab == 2) {
    Heading(Tr(TextId::TabGameFiles));
    Muted(Tr(TextId::FilesIntro));
    ImGui::Dummy(ImVec2(0, 16));
    BeginCard("Folder", 252);
    Heading(Tr(TextId::FolderHeading), 21);
    ImGui::TextColored(files_found ? kGreen : kWarning, "%s", Tr(files_found ? TextId::FilesDetected : TextId::StatusNotFound));
    ImGui::Spacing();
    // Scrollable path: long locations cannot overlap the actions.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::BeginChild("FolderPath", ImVec2(0, 48), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const std::string path = files_found ? PathText(game_root) : config.custom_game_root.empty() ? Tr(TextId::NoneDetected) : config.custom_game_root;
    ImGui::TextUnformatted(path.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar();
    Muted(Tr(TextId::FolderHint));
    ImGui::Spacing();
    ImGui::BeginDisabled(dialog_pending);
    if (PrimaryButton(Tr(TextId::BtnSelectFolder), ImVec2(-1, 38))) {
      { std::lock_guard lock(g_dialog_mutex); g_dialog_pending = true; }
      SDL_ShowOpenFolderDialog(FolderDialogCallback, nullptr, window, nullptr, false);
    }
    ImGui::EndDisabled();
    EndCard();
    const float half_w = (ImGui::GetContentRegionAvail().x - 10) / 2;
    if (ImGui::Button(Tr(TextId::BtnResetDefault), ImVec2(half_w, 36))) {
      config.custom_game_root.clear();
      status_message = Tr(TextId::PathResetMsg);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!files_found);
    if (ImGui::Button(Tr(TextId::BtnOpenFolder), ImVec2(half_w, 36))) OpenFolder(game_root);
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 14));
    BeginCard("SetupHelp", 140);
    ImGui::TextColored(kAccent, "%s", Tr(TextId::SetupHeading));
    ImGui::Spacing();
    Muted(Tr(TextId::SetupHint));
    EndCard();
  } else {
    Heading(Tr(TextId::TabDlc));
    Muted(Tr(TextId::DlcIntro));
    ImGui::Dummy(ImVec2(0, 16));

    // Loose DLC install card
    BeginCard("InstallDlc", 168);
    Heading(Tr(TextId::DlcInstallLoose), 21);
    Muted(Tr(TextId::DlcInstallLooseHint));
    ImGui::Spacing();
    const bool dlc_busy = [&]() {
      std::lock_guard lock(g_dlc_dialog_mutex);
      return g_dlc_dialog_pending;
    }();
    ImGui::BeginDisabled(dlc_busy);
    if (PrimaryButton(Tr(TextId::DlcSelectLooseFolder), ImVec2(-1, 38))) {
      { std::lock_guard lock(g_dlc_dialog_mutex); g_dlc_dialog_pending = true; }
      SDL_ShowOpenFolderDialog(DlcFolderDialogCallback, nullptr, window, nullptr, false);
    }
    ImGui::EndDisabled();
    EndCard();

    ImGui::Dummy(ImVec2(0, 14));

    // Installed DLC list card
    BeginCard("InstalledDlc", 220);
    Heading(Tr(TextId::DlcInstalledHeading), 21);
    const auto installed = launcher_content::ListInstalledDlc();
    if (installed.empty()) {
      Muted(Tr(TextId::DlcNoInstalled));
    } else {
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
      ImGui::BeginChild("DlcList", ImVec2(0, 110), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
      for (const auto& dlc : installed) {
        ImGui::BulletText("%s", dlc.name.c_str());
      }
      ImGui::EndChild();
      ImGui::PopStyleVar();
    }
    ImGui::Spacing();
    const float dlc_half_w = (ImGui::GetContentRegionAvail().x - 10) / 2;
    if (ImGui::Button(Tr(TextId::DlcPrepareFolders), ImVec2(dlc_half_w, 36))) {
      std::string error;
      if (launcher_content::EnsureDlcFolders(error)) {
        status_message = Tr(TextId::DlcFoldersReady);
      } else {
        status_message = error;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(TextId::DlcOpenContentFolder), ImVec2(dlc_half_w, 36))) {
      OpenFolder(launcher_content::GetDlcRoot());
    }
    EndCard();
  }
  ImGui::EndChild();
  ImGui::PopID();

  // Use the reserved geometry, not the tab selected mid-frame.
  if (actions_h > 0) {
    ImGui::Spacing();
    Muted(Tr(TextId::SettingsAppliedNotice));
    const float action_w = (available_w - 10) / 2;
    if (ImGui::Button(Tr(TextId::BtnResetRecommended), ImVec2(action_w, 38))) {
      ApplyRecommended(config);
      status_message = Tr(TextId::RecommendedApplied);
    }
    ImGui::SameLine();
    if (PrimaryButton(Tr(TextId::BtnSaveSettings), ImVec2(action_w, 38))) {
      status_message = Tr(ConfigManager::Save(base_dir / "narutorise.toml", config) ? TextId::SettingsSavedNotice : TextId::SaveFailed);
    }
  }
  ImGui::SetCursorPosY(size.y - 64);
  ImGui::Separator();
  ImGui::Spacing();
  if (!status_message.empty()) {
    ImGui::TextColored(kAccent, "%s", status_message.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", status_message.c_str());
  } else {
    Muted(Tr(TextId::FooterNotice));
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::End();
  return true;
}
