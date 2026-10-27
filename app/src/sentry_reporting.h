#pragma once

#include <filesystem>
#include <string>

namespace narutorise::reporting {
// Local Crashpad capture starts independently of upload consent. Every session
// owns a fresh database; an authorization never reopens an older session.
bool Initialize(const std::filesystem::path& executable_dir,
                const std::filesystem::path& cache_root, const std::string& version,
                bool consent);
bool SetConsent(bool consent);
bool CaptureFailure(const std::string& operation, const std::string& evidence);
void FinishStartup();
void Close();
// Developer-only test through the same application initialization as production.
// Returns false when reporting is unavailable or consent has not been given.
bool Verify(const std::string& mode);
}  // namespace narutorise::reporting
