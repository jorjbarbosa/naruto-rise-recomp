#pragma once

// The facts about a session that the crash reports carry: the graphics
// options, the runtime, the game data, the DLCs, the adapter and the role of
// each thread. Each fact goes to the incident's metadata and, with reporting
// compiled in, to the Sentry scope, whose copy on disk travels with a Crashpad
// minidump; so a crash carries the facts of its session. No paths or user
// names: the DLCs go by their package names (content hashes).
//
// The SDK's log adds two facts (log_facts.h): a FATAL becomes the tag and the
// fingerprint of the abort that follows it, and a module loaded with the
// devkit encryption key is listed.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace narutorise::report_context {
// At the start, before the game; the options all need a restart. A
// resolution scale of 0 follows the renderer's own options: "auto" until
// SetRenderMode gives the scale in use.
void SetGraphics(std::string_view renderer, std::string_view backend, int frame_rate, double resolution_scale,
                 int msaa_samples);
// The render mode the native renderer settled on (its scale and MSAA samples).
void SetRenderMode(double resolution_scale, int msaa_samples);
// Wine's version, or null on Windows and on Linux.
void SetRuntime(const char* wine_version);
void SetGameData(bool disc_image, uint32_t title_id, std::string_view xex_digest);
void SetContent(const std::vector<std::string>& packages);
// The engine's adapter once the native renderer runs: its API replaces the
// requested backend.
void SetAdapter(std::string_view api, std::string_view name, uint32_t vendor_id, uint32_t device_id,
                std::string_view driver);

// The calling thread's role (render, main, effects, engine, translate), once;
// a guest thread is also renamed "Guest <role> (handle)". The role of a thread
// that has none is "other". ThreadRole is safe in a crash handler.
void SetThreadRole(const char* role);
const char* ThreadRole() noexcept;

// From reporting::Initialize: the sink on the SDK's log, and the facts set so
// far into the new scope.
void InstallLogSink();
void Publish();
}  // namespace narutorise::report_context
