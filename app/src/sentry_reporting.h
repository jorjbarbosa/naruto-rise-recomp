#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace narutorise::reporting {
// Pending: the player has not answered yet. The destination is configured but
// nothing uploads until SetConsent(true) during the session.
enum class Consent { kRefused, kPending, kGiven };
// Local Crashpad capture starts independently of upload consent. Every session
// owns a fresh database; an authorization never reopens an older session.
bool Initialize(const std::filesystem::path& executable_dir,
                const std::filesystem::path& cache_root, const std::string& version,
                Consent consent);
// False when enabling needs a new session (refused at the start, or revoked).
bool SetConsent(bool consent);
// The SDK holds a scope (report_context.h writes the session's facts there).
bool Ready();
// The guest state at a renderer stop (gpu/host/stop.cpp), beside the
// operation and the evidence of the posted operation.
struct FailureDetails {
  uint64_t lr = 0;
  uint32_t guest_thread = 0, address = 0, value = 0;
  std::string shutdown;
};
// A renderer stop: grouped by its operation, with the detecting thread's stack.
bool CaptureFailure(const std::string& operation, const std::string& evidence,
                    const FailureDetails& details = {});
void FinishStartup();
void Close();
// Developer-only test through the same application initialization as production:
// error (a captured exception), crash (an abort) or fatal (the SDK's FATAL).
// Returns false when reporting is unavailable or consent has not been given.
bool Verify(const std::string& mode);
}  // namespace narutorise::reporting
