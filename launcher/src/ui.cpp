#include "ui.h"

#include <imgui.h>

#include <algorithm>
#include <iterator>
#include <utility>

#include "app_version.h"
#include "game_paths.h"
#include "localization.h"
#include "theme.h"
#include "widgets.h"

namespace ui = launcher_ui;

namespace {

struct Tab {
  ui::Page page;
  TextId label;
};

constexpr Tab kTabs[] = {
    {ui::Page::kPlay, TextId::TabPlay},
    {ui::Page::kSettings, TextId::TabSettings},
    {ui::Page::kGameFiles, TextId::TabGameFiles},
    {ui::Page::kDlc, TextId::TabDlc},
};

ui::GameStatus FindGame(const std::filesystem::path& base_dir, const GameConfig& config) {
  ui::GameStatus status;
  status.game_root = game_paths::Find(base_dir, config.custom_game_root);
#if defined(_WIN32)
  const auto executable = base_dir / "narutorise.exe";
#else
  const auto executable = base_dir / "narutorise";
#endif
  std::error_code error;
  status.executable_found = std::filesystem::is_regular_file(executable, error);
  return status;
}

void RenderLanguageSelector(GameConfig& config, float width) {
  ImGui::SetNextItemWidth(width);
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
}

void RenderFooter(const std::string& status_message, float y) {
  ImGui::SetCursorPosY(y);
  ImGui::Separator();
  ImGui::Spacing();
  if (!status_message.empty()) {
    ImGui::TextColored(ui::kAccent, "%s", status_message.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", status_message.c_str());
  } else {
    ui::Muted(Tr(TextId::FooterNotice));
  }
}

}  // namespace

LauncherUI::LauncherUI(SDL_Window* window, std::filesystem::path base_dir, Cover cover)
    : window_(window), base_dir_(std::move(base_dir)), cover_(cover), version_(AppVersion(base_dir_)) {
  RefreshTranslationMods();
}

void LauncherUI::RefreshTranslationMods() {
  translation_mods_ = ui::DiscoverTranslationMods(base_dir_);
}

void LauncherUI::SetupTheme() { launcher_theme::Apply(); }

void LauncherUI::Render(GameConfig& config, bool& request_launch, std::string& status_message) {
  Localization::SetLanguageCode(config.launcher_language);
  ui::ReconcileGameLanguage(config, translation_mods_);
  ui::GameStatus game;
  ui::PageContext context{window_, base_dir_, config, game, status_message, request_launch, page_,
                          translation_mods_};
  // A folder picked in a dialog counts in this frame's game status.
  ui::ApplyGameFolderSelection(context);
  ui::ApplyDlcFolderSelection(context);
  game = FindGame(base_dir_, config);

  // ImGui's display size is in logical coordinates, including on high-DPI displays.
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float cover_width = std::clamp(size.x * 0.35f, 330.0f, 440.0f);
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(size);
  ImGui::Begin("LauncherRoot", nullptr, ImGuiWindowFlags_NoDecoration |
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  RenderCover(cover_width, size.y);

  ImGui::SetCursorPos(ImVec2(cover_width, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));
  ImGui::BeginChild("ContentPanel", ImVec2(size.x - cover_width, size.y),
                    ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  RenderHeader(config);
  // The page fills the space down to the footer; its ID keeps each page's scroll apart.
  constexpr float kFooterHeight = 54;
  ImGui::PushID(static_cast<int>(page_));
  ImGui::BeginChild("PageBody", ImVec2(0, ImGui::GetContentRegionAvail().y - kFooterHeight));
  RenderPage(context);
  if (request_launch) {
    RefreshTranslationMods();
    ui::ReconcileGameLanguage(config, translation_mods_);
  }
  ImGui::EndChild();
  ImGui::PopID();
  RenderFooter(status_message, size.y - 64);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::End();
}

void LauncherUI::RenderCover(float width, float height) const {
  auto* draw = ImGui::GetWindowDrawList();
  if (cover_.texture && cover_.width > 0 && cover_.height > 0) {
    // Cover fit: crop symmetrically instead of stretching the artwork.
    const float image_aspect = static_cast<float>(cover_.width) / cover_.height;
    const float panel_aspect = width / height;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (panel_aspect < image_aspect) {
      const float visible = panel_aspect / image_aspect;
      uv0.x = (1 - visible) / 2; uv1.x = 1 - uv0.x;
    } else {
      const float visible = image_aspect / panel_aspect;
      uv0.y = (1 - visible) / 2; uv1.y = 1 - uv0.y;
    }
    draw->AddImage(reinterpret_cast<ImTextureID>(cover_.texture), ImVec2(0, 0), ImVec2(width, height), uv0, uv1);
  } else {
    draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(width, height),
        IM_COL32(59, 37, 27, 255), IM_COL32(31, 29, 35, 255),
        IM_COL32(17, 18, 23, 255), IM_COL32(27, 22, 22, 255));
    ImGui::SetCursorPos(ImVec2(28, height * 0.35f));
    ui::Heading("NARUTO", 38);
    ImGui::TextUnformatted("RISE OF A NINJA");
  }
  // A shade at the bottom keeps the version readable over the artwork.
  draw->AddRectFilledMultiColor(ImVec2(0, height - 100), ImVec2(width, height),
      IM_COL32(8, 9, 13, 0), IM_COL32(8, 9, 13, 0),
      IM_COL32(8, 9, 13, 230), IM_COL32(8, 9, 13, 230));
  draw->AddLine(ImVec2(width, 0), ImVec2(width, height), IM_COL32(122, 64, 31, 255));
  if (!version_.empty()) {
    ImGui::SetCursorPos(ImVec2(24, height - 40));
    ImGui::Text("v%s", version_.c_str());
  }
}

void LauncherUI::RenderHeader(GameConfig& config) {
  constexpr float kLanguageWidth = 130;
  // Each tab is as wide as its label, plus an equal share of the width left
  // beside the language selector.
  float label_widths[std::size(kTabs)];
  float spare = ImGui::GetContentRegionAvail().x - kLanguageWidth - 28;
  for (size_t i = 0; i < std::size(kTabs); ++i) {
    label_widths[i] = ImGui::CalcTextSize(Tr(kTabs[i].label)).x + ImGui::GetStyle().FramePadding.x * 2;
    spare -= label_widths[i];
  }
  const float share = std::max(0.0f, spare / std::size(kTabs));
  for (size_t i = 0; i < std::size(kTabs); ++i) {
    if (i) ImGui::SameLine(0, 4);
    ImGui::PushID(static_cast<int>(i));
    ImGui::PushStyleColor(ImGuiCol_Button, page_ == kTabs[i].page ? ui::kPrimary : ui::kIdleTab);
    if (ImGui::Button(Tr(kTabs[i].label), ImVec2(label_widths[i] + share, 38))) page_ = kTabs[i].page;
    ImGui::PopStyleColor();
    ImGui::PopID();
  }
  ImGui::SameLine(0, 16);
  RenderLanguageSelector(config, kLanguageWidth);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
}

void LauncherUI::RenderPage(ui::PageContext& context) const {
  switch (page_) {
    case ui::Page::kPlay: ui::RenderPlayPage(context); break;
    case ui::Page::kSettings: ui::RenderSettingsPage(context); break;
    case ui::Page::kGameFiles: ui::RenderGameFilesPage(context); break;
    case ui::Page::kDlc: ui::RenderDlcPage(context); break;
  }
}
