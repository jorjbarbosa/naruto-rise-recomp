#include "theme.h"

#include <imgui.h>

namespace launcher_theme {

void LoadFonts(const std::filesystem::path& base_dir) {
  ImGuiIO& io = ImGui::GetIO();
  const auto fonts = base_dir / "assets" / "fonts";
  const auto regular = fonts / "Lato-Regular.ttf";
  const auto bold = fonts / "Lato-Bold.ttf";
  const auto* glyph_ranges = io.Fonts->GetGlyphRangesCyrillic();
  // Bundle the same typeface on every machine; never rely on a Windows font.
  if (std::filesystem::exists(regular)) {
    io.FontDefault = io.Fonts->AddFontFromFileTTF(regular.string().c_str(), 17.0f, nullptr, glyph_ranges);
  }
  if (!io.FontDefault) io.FontDefault = io.Fonts->AddFontDefault();
  if (std::filesystem::exists(bold)) {
    io.Fonts->AddFontFromFileTTF(bold.string().c_str(), 17.0f, nullptr, glyph_ranges);
  }
}

void Apply() {
  ImGuiStyle& style = ImGui::GetStyle();
  ImVec4* colors = style.Colors;

  // Premium Dark Slate theme with Dante's Inferno / Naruto Chakra styling
  colors[ImGuiCol_Text] = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.60f, 1.00f);
  colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
  colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
  colors[ImGuiCol_Border] = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
  colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

  // Frames & Inputs
  colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.24f, 0.30f, 1.00f);

  // Naruto Chakra Orange accents
  const ImVec4 narutoOrange = ImVec4(0.95f, 0.42f, 0.05f, 1.00f);
  const ImVec4 narutoOrangeHover = ImVec4(1.00f, 0.52f, 0.15f, 1.00f);
  const ImVec4 narutoOrangeActive = ImVec4(0.80f, 0.35f, 0.03f, 1.00f);

  colors[ImGuiCol_CheckMark] = narutoOrange;
  colors[ImGuiCol_SliderGrab] = narutoOrange;
  colors[ImGuiCol_SliderGrabActive] = narutoOrangeActive;
  colors[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
  colors[ImGuiCol_ButtonHovered] = narutoOrangeHover;
  colors[ImGuiCol_ButtonActive] = narutoOrangeActive;
  colors[ImGuiCol_Header] = ImVec4(0.22f, 0.22f, 0.27f, 1.00f);
  colors[ImGuiCol_HeaderHovered] = narutoOrangeHover;
  colors[ImGuiCol_HeaderActive] = narutoOrangeActive;
  colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);

  style.WindowRounding = 0.0f;  // Clean borderless edge
   style.ChildRounding = 4.0f;
   style.FrameRounding = 3.0f;
  style.PopupRounding = 6.0f;
  style.GrabRounding = 4.0f;
  style.WindowBorderSize = 0.0f;
  style.FrameBorderSize = 1.0f;
  style.ChildBorderSize = 1.0f;
  style.ItemSpacing = ImVec2(10.0f, 8.0f);
   style.FramePadding = ImVec2(12.0f, 7.0f);
  style.WindowPadding = ImVec2(0.0f, 0.0f);
}

}  // namespace launcher_theme
