#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace narutorise::incident {
enum class Role : uint32_t { kUi, kGuest, kTranslator, kCompiler, kGpu, kCount };
// These calls never wait on an application mutex. Events are bounded; overflow
// and truncation are exposed in the manifest. Progress is allocation-free.
void Event(std::string_view operation, std::string_view state, std::string_view detail = {}, uint64_t operation_id = 0) noexcept;
uint64_t BeginOperation(std::string_view operation, std::string_view detail = {}) noexcept;
void Progress(Role role) noexcept;
void Active(Role role, bool active) noexcept;
class Activity {
 public:
  explicit Activity(Role role) : role_(role) { Active(role_, true); }
  ~Activity() { Progress(role_); Active(role_, false); }
  Activity(const Activity&) = delete;
  Activity& operator=(const Activity&) = delete;
 private:
  Role role_;
};
void FirstFailure(std::string_view operation, std::string_view evidence = {}) noexcept;
void ExpectedExit(std::string_view reason) noexcept;
void Collector(bool ready, std::string_view reason) noexcept;
bool CollectorReady() noexcept;
void SetMetadata(std::string_view key, std::string_view value) noexcept;
void SetLog(const std::filesystem::path& path) noexcept;
void RequestSnapshot() noexcept;
std::filesystem::path Directory();
std::filesystem::path ReportsDirectory();
std::string SessionId();
// The newest completed session other than this one; failures_only skips the
// normal exits, unseen_only also a failure Acknowledge already recorded.
std::filesystem::path Latest(bool failures_only = false, bool unseen_only = false);
// One line about a completed session: classification, exit status and whether
// an exception dump was captured.
std::string Summary(const std::filesystem::path& session);
void Acknowledge(const std::filesystem::path& session) noexcept;
bool VerifyIncident(const std::string& mode);
// A fixed snapshot of the published guest state, not a reconstruction of
// registers optimized into C++ locals. Safe to call from the SDK fault chain.
void GuestFault(uint64_t pc, uint64_t address, uint32_t kind, uint32_t thread,
                uint64_t base, const std::array<uint64_t, 32>& gpr,
                uint64_t lr, uint64_t ctr, uint32_t access_operation = 0) noexcept;
// Parent stays outside SDL/runtime. Child uses the unchanged SDK app lifecycle.
int RunSupervised(std::vector<std::string> arguments, const std::filesystem::path& executable,
                  const std::function<int(std::vector<std::string>&)>& run);
// ZIP export operates on a completed, immutable session; no private game data.
bool Export(const std::filesystem::path& session, const std::filesystem::path& destination,
            std::string& error);
void Prune(const std::filesystem::path& reports);
}  // namespace narutorise::incident
