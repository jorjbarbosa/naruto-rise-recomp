// The counters and the crash report facts of the native audio (diagnostics.h).
#include "audio/diagnostics.h"
#include "diagnostics/report_context.h"

#include <fmt/format.h>

namespace narutorise::audio::diagnostics {
namespace {
// JSON string content: quotes, backslashes and control characters escaped.
std::string Escape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += ch;
    } else if (static_cast<unsigned char>(ch) < 0x20) {
      out += fmt::format("\\u{:04x}", static_cast<unsigned char>(ch));
    } else {
      out += ch;
    }
  }
  return out;
}
}  // namespace

std::string ChannelJson(const ChannelUse& u) {
  return fmt::format(
      "{{\"frames\":{},\"active\":[{},{},{},{},{},{}],\"peak\":[{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}],"
      "\"fold_over\":{},\"fold_peak\":{:.4f}}}",
      u.frames, u.active[0], u.active[1], u.active[2], u.active[3], u.active[4], u.active[5], u.peak[0], u.peak[1],
      u.peak[2], u.peak[3], u.peak[4], u.peak[5], u.fold_over, u.fold_peak);
}

std::string Json(std::string_view backend, const OutputCounters& c) {
  const LayoutCheck& l = c.layout;
  return fmt::format(
      "{{\"backend\":\"{}\",\"device\":{{\"kind\":\"{}\",\"name\":\"{}\",\"driver\":\"{}\",\"channels\":{},"
      "\"endpoint_channels\":{},\"endpoint_frequency\":{},\"block_samples\":{}}},"
      "\"device_opens\":{},\"open_failures\":{},\"last_open_error\":\"{}\",\"device_lost\":{},\"device_stalls\":{},"
      "\"registers\":{},\"unregisters\":{},\"register_refused\":{},\"register_overflow\":{},"
      "\"unregister_unknown\":{},\"first_callback\":\"0x{:08X}\",\"first_argument\":\"0x{:08X}\","
      "\"callbacks\":{},\"periods\":{},\"double_periods\":{},\"skipped_periods\":{},\"grid_restarts\":{},"
      "\"queue_empty_periods\":{},\"queue_empty_closing\":{},\"queue_min\":{},"
      "\"submits\":{},\"frames_to_device\":{},\"frames_muted\":{},\"frames_invalid\":{},\"submit_unknown\":{},"
      "\"layout\":{{\"frames\":{},\"silent\":{},\"plausible\":{},\"implausible\":{},"
      "\"peak\":[{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}]}},"
      "\"thread_started\":{},\"thread_start_failed\":{},\"thread_joined\":{},\"shut_down\":{},\"channel_use\":{}}}",
      backend, c.device.kind, Escape(c.device.name), Escape(c.device.driver), c.device.channels,
      c.device.endpoint_channels, c.device.endpoint_frequency, c.device.block_samples, c.device_opens,
      c.open_failures, Escape(c.last_open_error), c.device_lost, c.device_stalls, c.registers, c.unregisters,
      c.register_refused, c.register_overflow, c.unregister_unknown, c.first_callback, c.first_argument,
      c.callbacks, c.periods, c.double_periods, c.skipped_periods, c.grid_restarts, c.queue_empty_periods,
      c.queue_empty_closing, c.queue_min, c.submits,
      c.frames_to_device, c.frames_muted, c.frames_invalid, c.submit_unknown, l.frames, l.silent, l.plausible,
      l.implausible, l.peak[0], l.peak[1], l.peak[2], l.peak[3], l.peak[4], l.peak[5], c.thread_started,
      c.thread_start_failed, c.thread_joined, c.shut_down, ChannelJson(c.channels));
}

void PublishBackend(std::string_view backend) { report_context::SetAudio(backend, {}, {}, {}, 0, 0, {}, 0); }

void PublishDevice(std::string_view backend, const OutputCounters& c) {
  report_context::SetAudio(backend, c.device.kind, c.device.name, c.device.driver, c.device.channels,
                           c.device.endpoint_channels, c.last_open_error, c.open_failures);
}
}  // namespace narutorise::audio::diagnostics
