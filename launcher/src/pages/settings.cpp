#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "graphics_devices.h"
#include "localization.h"
#include "pages/key_mapping.h"
#include "wine.h"
#include "pages/pages.h"
#include "pages/settings_form.h"
#include "widgets.h"

namespace launcher_ui {
namespace {

// --- Options -------------------------------------------------------------

constexpr Option kGameLanguage{TextId::GameLanguageLabel, TextId::GameLanguageDescription};
constexpr Option kSkipIntro{TextId::SkipIntroLabel, TextId::SkipIntroDescription};

constexpr Option kRenderer{TextId::RendererLabel, TextId::RendererDescription};
constexpr Option kGraphicsDevice{TextId::GraphicsDeviceLabel, TextId::GraphicsDeviceDescription};
constexpr Option kResolution{TextId::ResolutionLabel, TextId::ResolutionDescription};
constexpr Option kAspect{TextId::AspectLabel, TextId::AspectDescription, Renderer::kXenos};
constexpr Option kBrightness{TextId::BrightnessLabel, TextId::BrightnessDescription, Renderer::kNative};
constexpr Option kVsync{TextId::VsyncLabel, TextId::VsyncDescription, Renderer::kBoth,
                        TextId::VsyncNativeDescription};
constexpr Option kFrameRate{TextId::FrameRateLabel, TextId::FrameRateDescription, Renderer::kNative};
// From the game's 30.
constexpr int kFrameRates[] = {30, 60, 120};
constexpr Option kFullscreen{TextId::FullscreenLabel, TextId::FullscreenDescription};

constexpr Option kMsaa{TextId::MsaaLabel, TextId::MsaaDescription, Renderer::kNative};
constexpr Option kTransparencyAa{TextId::TransparencyAaLabel, TextId::TransparencyAaDescription,
                                 Renderer::kNative};
constexpr Option kFxaa{TextId::FxaaLabel, TextId::FxaaDescription};
constexpr Option kPostProcessing{TextId::PostProcessingLabel, TextId::PostProcessingDescription};
constexpr Option kTextureFiltering{TextId::TextureFilteringLabel, TextId::TextureFilteringDescription};
constexpr Option kDither{TextId::DitherLabel, TextId::DitherDescription, Renderer::kXenos};

constexpr Option kSdlInput{TextId::SdlInputLabel, TextId::SdlInputDescription};
constexpr Option kController{TextId::ControllerLabel, TextId::ControllerDescription};
constexpr Option kKeyboardMouse{TextId::KeyboardMouseLabel, TextId::KeyboardMouseDescription};
constexpr Option kMouseCamera{TextId::MouseCameraLabel, TextId::MouseCameraDescription};
constexpr Option kMouseSensitivity{TextId::MouseSensitivityLabel, TextId::MouseSensitivityDescription};
constexpr Option kKeyPrompts{TextId::KeyPromptsLabel, TextId::KeyPromptsDescription};
constexpr Option kKeys{TextId::KeysLabel, TextId::KeysDescription};

constexpr Option kFrameLatency{TextId::FrameLatencyLabel, TextId::FrameLatencyDescription, Renderer::kNative};
constexpr Option kVulkanFrameLatency{TextId::FrameLatencyLabel, TextId::NativeBackendDescription, Renderer::kNative};
constexpr Option kTripleBuffering{TextId::TripleBufferingLabel, TextId::TripleBufferingDescription,
                                  Renderer::kNative};
constexpr Option kFpsOverlay{TextId::FpsOverlayLabel, TextId::FpsOverlayDescription, Renderer::kXenos};

// The key mapping view, opened from the Controls group, takes the place of
// the groups, the options and the description.
bool g_mapping = false;

// --- Groups --------------------------------------------------------------

void RenderGameGroup(OptionList& list, PageContext& context) {
  auto& config = context.config;
  const auto languages = GameLanguages(config, context.translation_mods);
  std::vector<const char*> items;
  for (const auto& language : languages.options) items.push_back(language.label.c_str());
  int index = languages.selected;
  if (list.Combo(kGameLanguage, index, items) && index >= 0 && index < static_cast<int>(languages.options.size())) {
    SelectGameLanguage(config, languages.options[index]);
  }
  list.Checkbox(kSkipIntro, config.skip_intro_videos);
}

void RenderDisplayGroup(OptionList& list, PageContext& context) {
  auto& config = context.config;
  const bool native = kNativeRendererBuilt;
  if (native) {
    std::vector<const char*> names;
    std::vector<std::string> values;
    // Under Wine the game runs on Vulkan only (wine.h).
    if (kNativeD3d12Built && !(kNativeVulkanBuilt && narutorise::WineVersion())) {
      names.push_back("Direct3D 12");
      values.emplace_back("d3d12");
    }
    if (kNativeVulkanBuilt) { names.push_back("Vulkan"); values.emplace_back("vulkan"); }
    int backend = -1;
    for (size_t i = 0; i < values.size(); ++i) if (values[i] == config.native_backend) backend = int(i);
    if (list.Combo(kRenderer, backend, names) && backend >= 0) {
      config.renderer = "native";
      config.native_backend = values[backend];
      config.graphics_device.clear(); // Device names are enumerated separately by each API.
    }
  }

  // Automatic, the cards present, and a saved card that is no longer present.
  std::vector<const char*> devices = {Tr(TextId::GraphicsDeviceAuto)};
  int device = 0;
  for (const auto& name : GraphicsDevices(native && config.native_backend == "vulkan")) {
    if (name == config.graphics_device) device = static_cast<int>(devices.size());
    devices.push_back(name.c_str());
  }
  if (!config.graphics_device.empty() && device == 0) {
    device = static_cast<int>(devices.size());
    devices.push_back(config.graphics_device.c_str());
  }
  if (list.Combo(kGraphicsDevice, device, devices)) {
    config.graphics_device = device == 0 ? "" : std::string(devices[device]);
  }

  // The resolutions the game's 1280 x 720 becomes at each scale.
  if (native) {
    // The native renderer also takes a fractional scale (internal_resolution_scale).
    constexpr double kScales[] = {1.0, 1.5, 2.0, 3.0, 4.0};
    constexpr const char* kResolutions[] = {"1280x720", "1920x1080", "2560x1440", "3840x2160", "5120x2880"};
    const double current = config.internal_resolution_scale > 0 ? config.internal_resolution_scale
                                                                : config.resolution_scale;
    int scale = 0;
    for (int i = 1; i < 5; ++i) {
      if (std::abs(kScales[i] - current) < std::abs(kScales[scale] - current)) scale = i;
    }
    if (list.Combo(kResolution, scale, kResolutions)) {
      if (scale == 1) {
        config.internal_resolution_scale = kScales[1];
      } else {
        config.internal_resolution_scale = 0;
        config.resolution_scale = config.draw_resolution_scale_x = config.draw_resolution_scale_y =
            static_cast<int>(kScales[scale]);
      }
    }
  } else {
    constexpr const char* kResolutions[] = {"1280x720", "2560x1440", "3840x2160", "5120x2880"};
    int scale = std::clamp(config.resolution_scale - 1, 0, 3);
    if (list.Combo(kResolution, scale, kResolutions)) {
      config.resolution_scale = config.draw_resolution_scale_x = config.draw_resolution_scale_y = scale + 1;
    }
  }

  const char* aspects[] = {Tr(TextId::Aspect16x9), Tr(TextId::Aspect21x9), Tr(TextId::Aspect32x9)};
  int aspect = config.ultrawide_target_aspect >= 3.5 ? 2 : config.ultrawide_target_aspect >= 2.3 ? 1 : 0;
  if (list.Combo(kAspect, aspect, aspects)) {
    config.ultrawide_target_aspect = aspect == 2 ? 3.5556 : aspect == 1 ? 2.3889 : 0;
  }

  int brightness = std::clamp(static_cast<int>(std::lround(config.brightness * 100)), 0, 100);
  if (list.Slider(kBrightness, brightness, 0, 100)) config.brightness = brightness / 100.0;
  list.Slider(kFrameRate, config.frame_rate, kFrameRates);

  if (list.Checkbox(kVsync, config.vsync)) config.d3d12_host_vsync = config.vsync;
  list.Checkbox(kFullscreen, config.fullscreen);
}

void RenderQualityGroup(OptionList& list, PageContext& context) {
  auto& config = context.config;
  // msaa_samples 0 keeps the game's 4x.
  const char* msaa_modes[] = {"2x", Tr(TextId::MsaaOriginal), "8x"};
  int msaa = config.msaa_samples == 2 ? 0 : config.msaa_samples >= 8 ? 2 : 1;
  if (list.Combo(kMsaa, msaa, msaa_modes)) config.msaa_samples = msaa == 0 ? 2 : msaa == 2 ? 8 : 0;

  const char* fxaa_modes[] = {Tr(TextId::FxaaOff), Tr(TextId::FxaaNormal), Tr(TextId::FxaaExtreme)};
  int fxaa = config.swap_post_effect == "fxaa" ? 1 : config.swap_post_effect == "fxaa_extreme" ? 2 : 0;
  if (list.Combo(kFxaa, fxaa, fxaa_modes)) {
    config.swap_post_effect = fxaa == 1 ? "fxaa" : fxaa == 2 ? "fxaa_extreme" : "none";
  }

  const char* post_effects[] = {Tr(TextId::PostBilinear), Tr(TextId::PostCas), Tr(TextId::PostFsr)};
  int post = 0;
  if (config.present_effect == "cas" || config.present_effect == "cas_sharpen") post = 1;
  else if (config.present_effect == "fsr" || config.present_effect == "fsr_easu") post = 2;
  if (list.Combo(kPostProcessing, post, post_effects)) {
    config.present_effect = post == 1 ? "cas" : post == 2 ? "fsr" : "bilinear";
  }

  const char* filters[] = {Tr(TextId::AnisoDefault), "2x", "4x", "8x", "16x"};
  int filter = std::clamp(config.anisotropic_override - 1, 0, 4);
  if (list.Combo(kTextureFiltering, filter, filters)) config.anisotropic_override = filter + 1;

  list.Checkbox(kTransparencyAa, config.transparency_antialiasing);
  list.Checkbox(kDither, config.present_dither);
}

std::string ControllerName() {
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  std::string name = Tr(TextId::NoControllerDetected);
  if (ids && count > 0) {
    const char* sdl_name = SDL_GetGamepadNameForID(ids[0]);
    name = sdl_name ? sdl_name : "Gamepad";
  }
  SDL_free(ids);
  return name;
}

void RenderControlsGroup(OptionList& list, PageContext& context) {
  auto& config = context.config;
  bool sdl = config.input_backend != "xinput";
  if (list.Checkbox(kSdlInput, sdl)) config.input_backend = sdl ? "sdl" : "xinput";
  list.Text(kController, ControllerName().c_str());
  list.Checkbox(kKeyboardMouse, config.mnk_mode);
  // The mouse and the keys count only in keyboard and mouse mode.
  ImGui::BeginDisabled(!config.mnk_mode);
  list.Checkbox(kMouseCamera, config.mnk_mouse);
  // 10% to 300%; a value set by hand outside them stays until the slider moves.
  int sensitivity = std::clamp(static_cast<int>(std::lround(config.mnk_sensitivity * 100)), 10, 300);
  if (list.Slider(kMouseSensitivity, sensitivity, 10, 300, "%d%%")) config.mnk_sensitivity = sensitivity / 100.0;
  list.Checkbox(kKeyPrompts, config.mnk_key_prompts);
  if (list.Button(kKeys, Tr(TextId::MapKeysButton))) g_mapping = true;
  ImGui::EndDisabled();
}

void RenderAdvancedGroup(OptionList& list, PageContext& context) {
  auto& config = context.config;
  const char* latencies[] = {Tr(TextId::FrameLatencyLowest), "2", "3"};
  int latency = std::clamp(config.max_frame_latency, 1, 3) - 1;
  if (kNativeRendererBuilt && config.native_backend == "vulkan")
    list.Text(kVulkanFrameLatency, latencies[0]);
  else if (list.Combo(kFrameLatency, latency, latencies)) config.max_frame_latency = latency + 1;
  list.Checkbox(kTripleBuffering, config.triple_buffering);
  list.Checkbox(kFpsOverlay, config.show_fps_overlay);
}

struct Group {
  TextId label;
  void (*render)(OptionList& list, PageContext& context);
};

constexpr Group kGroups[] = {
    {TextId::GroupGame, RenderGameGroup},
    {TextId::GroupDisplay, RenderDisplayGroup},
    {TextId::GroupQuality, RenderQualityGroup},
    {TextId::GroupControls, RenderControlsGroup},
    {TextId::GroupAdvanced, RenderAdvancedGroup},
};

size_t g_group = 0;

// --- Page parts ----------------------------------------------------------

void ApplyRecommended(GameConfig& config) {
  // The resolution and the aspect ratio of the game itself, as Xenia defaults them.
  config.resolution_scale = config.draw_resolution_scale_x = config.draw_resolution_scale_y = 1;
  config.vsync = config.d3d12_host_vsync = config.fullscreen = config.skip_intro_videos = true;
  config.anisotropic_override = 5;
  config.swap_post_effect = "fxaa";
  config.present_effect = "cas";
  config.present_dither = false;
  config.show_fps_overlay = false;
  config.msaa_samples = 0;
  config.transparency_antialiasing = config.triple_buffering = false;
  config.frame_rate = 60;
  config.brightness = 0.5;
  config.internal_resolution_scale = 0;
  config.max_frame_latency = 1;
  config.input_backend = "sdl";
  config.ultrawide_target_aspect = 0;
}

// One entry of the group list: the active one is lit, with a bar in the accent color.
bool GroupButton(const char* label, bool active) {
  constexpr float kHeight = 36;
  const ImVec2 min = ImGui::GetCursorScreenPos();
  const ImVec2 max(min.x + ImGui::GetContentRegionAvail().x, min.y + kHeight);
  const bool clicked = ImGui::InvisibleButton(label, ImVec2(max.x - min.x, kHeight));
  auto* draw = ImGui::GetWindowDrawList();
  if (active || ImGui::IsItemHovered()) {
    draw->AddRectFilled(min, max, ImGui::GetColorU32(active ? ImGuiCol_Header : ImGuiCol_FrameBgHovered), 3);
  }
  if (active) draw->AddRectFilled(min, ImVec2(min.x + 3, max.y), ImGui::ColorConvertFloat4ToU32(kPrimary));
  auto& fonts = ImGui::GetIO().Fonts->Fonts;
  ImFont* font = active && fonts.Size > 1 ? fonts[1] : ImGui::GetFont();
  const float size = ImGui::GetFontSize();
  draw->AddText(font, size, ImVec2(min.x + 16, min.y + (kHeight - size) / 2),
                ImGui::GetColorU32(active ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
  return clicked;
}

void RenderGroupList(float height) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
  ImGui::BeginChild("Groups", ImVec2(150, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  for (size_t i = 0; i < std::size(kGroups); ++i) {
    if (GroupButton(Tr(kGroups[i].label), g_group == i)) g_group = i;
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

// The options of the active group; returns the one to describe.
const Option* RenderOptions(PageContext& context, float height) {
  const bool native = kNativeRendererBuilt;
  const Group& group = kGroups[g_group];
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));
  ImGui::BeginChild("Options", ImVec2(0, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  Heading(Tr(group.label), 17);
  ImGui::PopStyleColor();
  ImGui::Spacing();
  const Option* described = nullptr;
  {
    OptionList list(native);
    group.render(list, context);
    described = list.Described();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  return described;
}

void RenderDescriptionPanel(const Option* option, bool native, float height) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 12));
  ImGui::BeginChild("Description", ImVec2(0, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  if (option) RenderDescription(*option, native);
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

constexpr float kActionButtonHeight = 38;

float ActionsHeight() {
  return ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y + kActionButtonHeight;
}

void RenderActions(PageContext& context) {
  Muted(Tr(TextId::SettingsAppliedNotice));
  const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
  if (ImGui::Button(Tr(TextId::BtnResetRecommended), ImVec2(width, kActionButtonHeight))) {
    ApplyRecommended(context.config);
    context.status_message = Tr(TextId::RecommendedApplied);
  }
}

void RenderKeyMappingActions(PageContext& context) {
  Muted(Tr(TextId::SettingsAppliedNotice));
  const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
  if (ImGui::Button(Tr(TextId::KeysRestoreDefaults), ImVec2(width, kActionButtonHeight))) {
    RestoreDefaultKeys(context);
  }
  ImGui::SameLine();
  if (PrimaryButton(Tr(TextId::KeysDone), ImVec2(width, kActionButtonHeight))) g_mapping = false;
}

}  // namespace

void RenderSettingsPage(PageContext& context) {
  if (g_mapping) {
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    RenderKeyMapping(context, std::max(160.0f, ImGui::GetContentRegionAvail().y - ActionsHeight() - spacing));
    RenderKeyMappingActions(context);
    return;
  }
  // The group list and the options on top, the description of the pointed
  // option below them, then the actions; the options take the height left.
  constexpr float kDescriptionHeight = 124;
  const float spacing = ImGui::GetStyle().ItemSpacing.y;
  const float options_height = std::max(
      160.0f, ImGui::GetContentRegionAvail().y - kDescriptionHeight - ActionsHeight() - 2 * spacing);
  const bool native = kNativeRendererBuilt;

  RenderGroupList(options_height);
  ImGui::SameLine();
  const Option* described = RenderOptions(context, options_height);
  RenderDescriptionPanel(described, native, kDescriptionHeight);
  RenderActions(context);
}

}  // namespace launcher_ui
