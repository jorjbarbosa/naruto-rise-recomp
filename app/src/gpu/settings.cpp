// The graphics options of the native renderer (settings.h): the cvars of the
// Xenos plugin registered here under the SDK names when the native renderer
// runs, the presenter cvars of rexruntime.dll read by name, and the journal
// fields of the options in use.
#include "gpu/settings.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/host/services.h"
#include "wine.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <deque>
#include <functional>
#include <string_view>
#include <vector>
#include <mutex>
#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

// Options of the native renderer without an SDK counterpart: the swapchain
// latency and buffer count. The defaults: two buffers, frame latency 1.
REXCVAR_DEFINE_INT32(max_frame_latency, 1, "narutorise/Graphics",
                     "Frames the native renderer may queue ahead of the display (1 = lowest latency)")
    .range(1, 3)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(triple_buffering, false, "narutorise/Graphics",
                    "Three swapchain buffers in the native renderer instead of two")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Brightness: an exponent on the title's gamma ramp.
REXCVAR_DEFINE_DOUBLE(brightness, 0.5, "narutorise/Graphics",
                      "Brightness of the native renderer's output (0.5 = the game's own)")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// A fractional internal scale, which the integer options of the SDK cannot
// give (1.5 turns the title's 1280 x 720 into 1920 x 1080). 0 leaves the scale
// to resolution_scale.
// The sample count of the targets the title creates with
// MSAA (the 4X pass and its tile backing here); single-sample targets stay so.
REXCVAR_DEFINE_INT32(msaa_samples, 0, "narutorise/Graphics",
                     "MSAA samples of the game's multisampled targets in the native renderer: 0 (the game's), "
                     "2, 4 or 8, limited to the device")
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Transparency antialiasing: the alpha test of a draw into
// a multisampled target becomes alpha to coverage (shaders.h, TransparencySpec).
REXCVAR_DEFINE_BOOL(transparency_antialiasing, false, "narutorise/Graphics",
                    "Alpha to coverage instead of the alpha test in the native renderer's multisampled targets")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// The frame rate of the native renderer: above 30, the state interpolation
// (gpu/interpolation/tick.h) keeps the game's logic at its own step and draws
// the frames between the steps. 30 keeps the game's frame rate; the product's
// configuration and the launcher choose 60.
REXCVAR_DEFINE_INT32(frame_rate, 30, "narutorise/Graphics",
                     "Frame rate of the native renderer: 30 keeps the game's; 60 to 240 interpolate the frames "
                     "between the game's logic steps")
    .range(30, 240)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(internal_resolution_scale, 0.0, "narutorise/Graphics",
                      "Internal resolution scale of the native renderer, 1 to 4 with fractions "
                      "(0 = resolution_scale)")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// Defined by the app for both renderers (graphics_device.cpp).
REXCVAR_DECLARE(std::string, graphics_device);
#if defined(NARUTORISE_NATIVE_D3D12_DISABLED)
REXCVAR_DEFINE_STRING(native_backend, "vulkan", "narutorise/Graphics", "Native renderer graphics API")
#else
REXCVAR_DEFINE_STRING(native_backend, "d3d12", "narutorise/Graphics", "Native renderer graphics API")
#endif
    .allowed({"d3d12", "vulkan"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace gpu {
namespace {
Settings settings;
Settings requested_settings;
std::mutex settings_mutex;
uint64_t requested_revision = 0;
SettingsResult last_result;
std::string evidence;

// The options that the SDK defines in the Xenos plugin (command_processor.cpp,
// graphics_system.cpp, texture/cache.cpp), with its names, ranges and allowed
// values. The executable also runs the plugin, which registers them itself
// when it loads, and the registry keeps the first owner of a name: the native
// renderer registers them only when it is chosen (RegisterPluginOptions). The
// registry then applies the configured value without the range check, as for
// the plugin; LoadSettings clamps. anisotropic_override keeps the title's
// filter by default instead of the SDK's 4x.
struct PluginOptions {
  bool vsync = true;
  int32_t anisotropic_override = -1;
  std::string swap_post_effect = "none";
  int32_t draw_resolution_scale_x = 1, draw_resolution_scale_y = 1, resolution_scale = 1;
};
PluginOptions plugin;

rex::cvar::FlagEntry PluginOption(const char* name, rex::cvar::FlagType type, const char* description,
                                  std::function<bool(std::string_view)> set, std::function<std::string()> get) {
  rex::cvar::FlagEntry entry;
  entry.name = name;
  entry.type = type;
  entry.category = "GPU";
  entry.description = description;
  entry.setter = std::move(set);
  entry.getter = std::move(get);
  entry.command_callback = [](std::string_view) {};
  entry.lifecycle = rex::cvar::Lifecycle::kRequiresRestart;
  entry.default_value = entry.getter();
  return entry;
}
rex::cvar::FlagEntry BoolOption(const char* name, bool& value, const char* description) {
  return PluginOption(name, rex::cvar::FlagType::Boolean, description,
                      [&value](std::string_view v) {
                        value = rex::string::from_string<bool>(v, false);
                        return true;
                      },
                      [&value] { return std::string(value ? "true" : "false"); });
}
rex::cvar::FlagEntry IntOption(const char* name, int32_t& value, int32_t min, int32_t max, const char* description) {
  auto entry = PluginOption(name, rex::cvar::FlagType::Int32, description,
                            [&value](std::string_view v) {
                              int32_t parsed = 0;
                              if (std::from_chars(v.data(), v.data() + v.size(), parsed).ec != std::errc()) return false;
                              value = parsed;
                              return true;
                            },
                            [&value] { return std::to_string(value); });
  entry.constraints.min = min;
  entry.constraints.max = max;
  return entry;
}
rex::cvar::FlagEntry StringOption(const char* name, std::string& value, std::vector<std::string> allowed,
                                  const char* description) {
  auto entry = PluginOption(name, rex::cvar::FlagType::String, description,
                            [&value](std::string_view v) {
                              value = std::string(v);
                              return true;
                            },
                            [&value] { return value; });
  entry.constraints.allowed_values = std::move(allowed);
  return entry;
}

// The options of the record: the first fourteen defined in the app, the rest by the presenter.
constexpr std::string_view kOptions[] = {
    "vsync", "anisotropic_override", "swap_post_effect", "resolution_scale", "draw_resolution_scale_x",
    "draw_resolution_scale_y", "max_frame_latency", "triple_buffering", "brightness", "internal_resolution_scale",
    "msaa_samples", "transparency_antialiasing", "frame_rate", "graphics_device", "native_backend",
    "d3d12_allow_variable_refresh_rate_and_tearing", "present_effect", "present_cas_additional_sharpness",
    "present_fsr_sharpness_reduction", "present_fsr_max_upsampling_passes"};

const char* SourceName(std::string_view name) {
  if (!rex::cvar::GetFlagInfo(name)) return "missing";
  switch (rex::cvar::GetFlagSource(name)) {
    case rex::cvar::Source::kDefault: return "default";
    case rex::cvar::Source::kConfig: return "config";
    case rex::cvar::Source::kEnvironment: return "environment";
    case rex::cvar::Source::kCommandLine: return "command-line";
    case rex::cvar::Source::kRuntime: return "runtime";
  }
  return "unknown";
}
std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}
// A JSON string of a configured value: the config file may hold any text.
std::string Quoted(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    out += static_cast<unsigned char>(c) < 32 ? '?' : c;
  }
  return out + '"';
}
// A presenter option, or the presenter's default when this rexruntime.dll was
// built without it (present_effect has the CAS and FSR values only with
// REX_HAS_FIDELITYFX_SDK).
float PresenterFloat(std::string_view name, float fallback, float min, float max) {
  if (!rex::cvar::GetFlagInfo(name)) return fallback;
  const double value = rex::cvar::Query<double>(name);
  return std::isfinite(value) ? std::clamp(float(value), min, max) : fallback;
}
const char* Name(PresentEffect effect) {
  switch (effect) {
    case PresentEffect::kCas: return "cas";
    case PresentEffect::kFsr: return "fsr";
    default: return "bilinear";
  }
}
const char* Name(SwapPostEffect effect) {
  switch (effect) {
    case SwapPostEffect::kFxaa: return "fxaa";
    case SwapPostEffect::kFxaaExtreme: return "fxaa_extreme";
    default: return "none";
  }
}
}  // namespace

void RegisterPluginOptions() {
  static std::deque<rex::cvar::FlagRegistrar> registered;
  if (!registered.empty()) return;
  registered.emplace_back(BoolOption("vsync", plugin.vsync, "Enable vertical sync"));
  registered.emplace_back(IntOption("anisotropic_override", plugin.anisotropic_override, -1, 5,
                                    "Forces anisotropic filtering for eligible textures.\n"
                                    " -1 = No override (the title's filter)\n"
                                    "  0 = Disable anisotropic filtering\n"
                                    "  1 to 5 = Force 1x, 2x, 4x, 8x, 16x anisotropic filtering"));
  registered.emplace_back(StringOption("swap_post_effect", plugin.swap_post_effect, {"none", "fxaa", "fxaa_extreme"},
                                       "Swap post effect: none, fxaa, fxaa_extreme"));
  registered.emplace_back(IntOption("draw_resolution_scale_x", plugin.draw_resolution_scale_x, 1, 8,
                                    "Draw resolution scale X (1 = no scaling)"));
  registered.emplace_back(IntOption("draw_resolution_scale_y", plugin.draw_resolution_scale_y, 1, 8,
                                    "Draw resolution scale Y (1 = no scaling)"));
  registered.emplace_back(IntOption("resolution_scale", plugin.resolution_scale, 1, 8,
                                    "Draw resolution scale for both X and Y axes (same as setting "
                                    "draw_resolution_scale_x and draw_resolution_scale_y)"));
}

void LoadSettings() {
  Settings s;
  s.vsync = plugin.vsync;
  s.anisotropic_override = std::clamp(plugin.anisotropic_override, -1, 5);
  const std::string swap_effect = Lower(plugin.swap_post_effect);
  s.swap_post_effect = swap_effect == "fxaa" ? SwapPostEffect::kFxaa
      : swap_effect == "fxaa_extreme" ? SwapPostEffect::kFxaaExtreme : SwapPostEffect::kNone;
  // As TextureCache::GetConfigDrawResolutionScale: resolution_scale sets the axes
  // whose own option keeps its default. internal_resolution_scale, when set,
  // takes both axes. Up to 4x: the 4X MSAA backing of the pass alone is about
  // 700 MB there, and the engine's surfaces stop at 8192.
  const bool shared = rex::cvar::HasNonDefaultValue("resolution_scale");
  const auto axis = [&](int32_t own, std::string_view own_name) {
    const int32_t value = shared && !rex::cvar::HasNonDefaultValue(own_name) ? plugin.resolution_scale : own;
    return float(std::clamp(value, 1, 4));
  };
  const double internal_scale = REXCVAR_GET(internal_resolution_scale);
  if (std::isfinite(internal_scale) && internal_scale > 0) {
    s.resolution_scale.x = s.resolution_scale.y = std::clamp(float(internal_scale), 1.0f, 4.0f);
  } else {
    s.resolution_scale.x = axis(plugin.draw_resolution_scale_x, "draw_resolution_scale_x");
    s.resolution_scale.y = axis(plugin.draw_resolution_scale_y, "draw_resolution_scale_y");
  }
  s.max_frame_latency = uint32_t(std::clamp(REXCVAR_GET(max_frame_latency), 1, 3));
  s.triple_buffering = REXCVAR_GET(triple_buffering);
  const double brightness = REXCVAR_GET(brightness);
  s.brightness = std::isfinite(brightness) ? std::clamp(float(brightness), 0.0f, 1.0f) : 0.5f;
  // A value between the allowed ones takes the one below it.
  const int32_t msaa = REXCVAR_GET(msaa_samples);
  s.msaa_samples = msaa >= 8 ? 8 : msaa >= 4 ? 4 : msaa >= 2 ? 2 : 0;
  s.transparency_antialiasing = REXCVAR_GET(transparency_antialiasing);
  s.frame_rate = uint32_t(std::clamp(REXCVAR_GET(frame_rate), 30, 240));
  s.graphics_device = REXCVAR_GET(graphics_device);
  s.native_backend = REXCVAR_GET(native_backend);
#if defined(NARUTORISE_NATIVE_VULKAN)
  // Under Wine (Proton), Direct3D 12 runs through vkd3d-proton, and the game
  // closes there at startup; Vulkan goes to the driver directly. So Vulkan,
  // whatever the configuration says.
  if (narutorise::WineVersion()) s.native_backend = "vulkan";
#endif
  if (rex::cvar::GetFlagInfo("d3d12_allow_variable_refresh_rate_and_tearing"))
    s.allow_tearing = rex::cvar::Query<bool>("d3d12_allow_variable_refresh_rate_and_tearing");
  // fsr2 and fsr3 are the SDK's temporal upscalers, not covered: they keep bilinear.
  const std::string requested =
      rex::cvar::GetFlagInfo("present_effect") ? Lower(rex::cvar::Query<std::string>("present_effect")) : "bilinear";
  s.present_effect = requested == "cas" ? PresentEffect::kCas
      : requested == "fsr" ? PresentEffect::kFsr : PresentEffect::kBilinear;
  s.cas_additional_sharpness = PresenterFloat("present_cas_additional_sharpness", 0.0f, 0.0f, 1.0f);
  s.fsr_sharpness_reduction = PresenterFloat("present_fsr_sharpness_reduction", 0.2f, 0.0f, 2.0f);
  s.fsr_max_passes = uint32_t(PresenterFloat("present_fsr_max_upsampling_passes", 4.0f, 1.0f, 4.0f));
  { std::lock_guard lock(settings_mutex); settings = requested_settings = s; }

  std::string sources;
  for (const auto name : kOptions)
    sources += fmt::format("{}\"{}\":\"{}\"", sources.empty() ? "" : ",", name, SourceName(name));
  evidence = fmt::format(
      "\"vsync\":{},\"allow_tearing\":{},\"anisotropic_override\":{},\"present_effect\":\"{}\","
      "\"present_effect_requested\":{},\"cas_additional_sharpness\":{},\"fsr_sharpness_reduction\":{},"
      "\"swap_post_effect\":\"{}\",\"resolution_scale\":[{},{}],\"max_frame_latency\":{},\"triple_buffering\":{},"
      "\"brightness\":{},\"fsr_max_passes\":{},\"msaa_samples\":{},\"transparency_antialiasing\":{},"
      "\"frame_rate\":{},\"graphics_device\":{},\"sources\":{{{}}}",
      s.vsync, s.allow_tearing, s.anisotropic_override, Name(s.present_effect), Quoted(requested),
      s.cas_additional_sharpness, s.fsr_sharpness_reduction, Name(s.swap_post_effect), s.resolution_scale.x,
      s.resolution_scale.y, s.max_frame_latency, s.triple_buffering, s.brightness, s.fsr_max_passes, s.msaa_samples,
      s.transparency_antialiasing, s.frame_rate, Quoted(s.graphics_device), sources);
}
Settings CurrentSettings() { std::lock_guard lock(settings_mutex); return settings; }
Settings RequestedSettings() { std::lock_guard lock(settings_mutex); return requested_settings; }
uint64_t RequestSettings(Settings next) {
  std::lock_guard lock(settings_mutex);
  // The API and adapter belong to the session, even after a saved restart edit.
  next.native_backend = settings.native_backend;
  next.graphics_device = settings.graphics_device;
  requested_settings = std::move(next);
  return ++requested_revision;
}
SettingsResult LastSettingsResult() { std::lock_guard lock(settings_mutex); return last_result; }
void ApplyPendingSettings(Dispatcher& dispatcher) {
  Settings next;
  uint64_t revision;
  {
    std::lock_guard lock(settings_mutex);
    if (requested_revision == last_result.revision) return;
    revision = requested_revision;
    next = requested_settings;
  }
  PresentMode present;
  present.vsync = next.vsync; present.allow_tearing = next.allow_tearing;
  present.buffers = next.triple_buffering ? 3 : 2;
  present.max_frame_latency = next.max_frame_latency;
  present.brightness = next.brightness; present.effect = next.present_effect;
  present.post_effect = next.swap_post_effect; present.cas_sharpness = next.cas_additional_sharpness;
  present.fsr_sharpness_reduction = next.fsr_sharpness_reduction; present.fsr_passes = next.fsr_max_passes;
  RenderMode render{next.resolution_scale, next.msaa_samples, next.graphics_device, dispatcher.Api()};
  std::string detail;
  const bool applied = dispatcher.Run([render, present, next](Engine& engine, std::string& out) {
    if (!engine.Reconfigure(render, present, out)) return false;
    // Publish before the stage may translate any operation after this Run.
    std::lock_guard lock(settings_mutex);
    settings = next;
    return true;
  }, detail);
  {
    std::lock_guard lock(settings_mutex);
    if (!applied && requested_revision == revision) requested_settings = settings;
    last_result = {revision, applied, detail};
  }
  host::Record("native-live-settings", fmt::format("\"revision\":{},\"applied\":{},\"frame_rate\":{},"
      "\"anisotropic_override\":{},\"transparency_antialiasing\":{},\"engine\":{}", revision, applied,
      next.frame_rate, next.anisotropic_override, next.transparency_antialiasing, detail.empty() ? "null" : detail));
  REXLOG_INFO("[narutorise] Live settings revision {}: applied={}, frame_rate={}, anisotropy={}, transparency_aa={}, {}",
      revision, applied, next.frame_rate, next.anisotropic_override, next.transparency_antialiasing, detail);
}
std::string SettingsEvidence() { return evidence; }
}  // namespace gpu
