#pragma once

// What the native audio tells about itself: the counters as one JSON object
// (in the log at the close and once the callback has run for a few seconds),
// and the facts the crash reports carry (report_context.h): the backend, the
// device and why it did not open.
#include "audio/output.h"

#include <string>
#include <string_view>

namespace narutorise::audio::diagnostics {
std::string Json(std::string_view backend, const OutputCounters& counters);
// The backend alone (the SDK's audio, or before the device is known).
void PublishBackend(std::string_view backend);
void PublishDevice(std::string_view backend, const OutputCounters& counters);
}  // namespace narutorise::audio::diagnostics
