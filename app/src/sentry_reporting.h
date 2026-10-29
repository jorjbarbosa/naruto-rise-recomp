#pragma once

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
bool CaptureFailure(const std::string& operation, const std::string& evidence);
void FinishStartup();
void Close();
// Developer-only test through the same application initialization as production.
// Returns false when reporting is unavailable or consent has not been given.
bool Verify(const std::string& mode);
}  // namespace narutorise::reporting
