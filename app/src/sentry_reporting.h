#pragma once

#include <filesystem>
#include <string>

namespace narutorise::reporting {
// Uploads require the persisted, explicit crash_reporting preference. When
// disabled at startup, neither the SDK nor its background handler is started.
bool Initialize(const std::filesystem::path& executable_dir,
                const std::filesystem::path& cache_root, const std::string& version,
                bool consent);
bool SetConsent(bool consent);
void FinishStartup();
void Close();
// Developer-only test through the same application initialization as production.
// Returns false when reporting is unavailable or consent has not been given.
bool Verify(const std::string& mode);
}  // namespace narutorise::reporting
