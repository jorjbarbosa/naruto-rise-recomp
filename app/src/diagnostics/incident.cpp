#include "diagnostics/incident.h"
#include "diagnostics/channel.h"
#include "gpu/common/sha256.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <new>
#include <nlohmann/json.hpp>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace narutorise::incident {
namespace fs = std::filesystem;
namespace {
Mapping client;
fs::path directory;
using Json = nlohmann::json;
std::string Dump(const Json& value) { return value.dump(-1, ' ', false, Json::error_handler_t::replace); }
void Publish(std::string_view text) noexcept {
  if (!client.channel) return;
  auto& c = *client.channel;
  const uint64_t n = c.next.fetch_add(1, std::memory_order_relaxed);
  if (text.size() >= kPayload) c.truncated.fetch_add(1, std::memory_order_relaxed);
  if (!WriteSlot(c.events[n % kSlots], (n + 1) * 2, text)) c.dropped.fetch_add(1, std::memory_order_relaxed);
}
}
uint64_t Now() noexcept {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint32_t Pid() noexcept {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return uint32_t(getpid());
#endif
}
uint64_t ThreadId() noexcept {
#ifdef _WIN32
  return GetCurrentThreadId();
#else
  return uint64_t(syscall(SYS_gettid));
#endif
}
std::string Utf8(const fs::path& path) {
  const auto value = path.u8string(); return {value.begin(), value.end()};
}
bool Mapping::Open(const fs::path& path, bool create) {
#ifdef _WIN32
  file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     nullptr, create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) { file = nullptr; return false; }
  if (!create) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart != sizeof(Channel)) return false;
  }
  mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, DWORD(sizeof(Channel)), nullptr);
  if (!mapping) return false;
  channel = static_cast<Channel*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Channel)));
#else
  fd = open(path.c_str(), O_RDWR | (create ? O_CREAT | O_EXCL : 0), 0600);
  if (fd < 0 || (create && ftruncate(fd, sizeof(Channel)) != 0)) return false;
  struct stat info{};
  if (fstat(fd, &info) || info.st_size != sizeof(Channel)) return false;
  void* view = mmap(nullptr, sizeof(Channel), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (view != MAP_FAILED) channel = static_cast<Channel*>(view);
#endif
  if (!channel) return false;
  if (create) { new (channel) Channel{}; channel->parent_pid = Pid(); }
  return channel->version == kChannelVersion;
}
Mapping::~Mapping() {
#ifdef _WIN32
  if (channel) UnmapViewOfFile(channel);
  if (mapping) CloseHandle(mapping);
  if (file) CloseHandle(file);
#else
  if (channel) munmap(channel, sizeof(Channel));
  if (fd >= 0) close(fd);
#endif
}
bool WriteSlot(Slot& slot, uint64_t version, std::string_view text) noexcept {
  auto old = slot.version.load(std::memory_order_acquire);
  if ((old & 1) || old >= version || !slot.version.compare_exchange_strong(old, version - 1, std::memory_order_acq_rel)) return false;
  slot.time.store(Now(), std::memory_order_relaxed);
  slot.thread.store(ThreadId(), std::memory_order_relaxed);
  const size_t size = std::min(text.size(), size_t(kPayload - 1));
  for (size_t i = 0; i < size; ++i) slot.bytes[i].store(text[i], std::memory_order_relaxed);
  slot.bytes[size].store(0, std::memory_order_relaxed);
  slot.version.store(version, std::memory_order_release);
  return true;
}
bool ReadSlot(const Slot& slot, uint64_t& version, uint64_t& time, uint64_t& thread, std::string& text) {
  version = slot.version.load(std::memory_order_acquire);
  if (!version || (version & 1)) return false;
  time = slot.time.load(std::memory_order_relaxed); thread = slot.thread.load(std::memory_order_relaxed);
  text.clear();
  for (const auto& byte : slot.bytes) { const char c = byte.load(std::memory_order_relaxed); if (!c) break; text += c; }
  std::atomic_thread_fence(std::memory_order_acquire);
  return slot.version.load(std::memory_order_relaxed) == version;
}
void Event(std::string_view operation, std::string_view state, std::string_view detail, uint64_t operation_id) noexcept {
  if (!client.channel) return;
  try { Publish(Dump(Json{{"operation", operation}, {"operation_id", operation_id}, {"state", state}, {"detail", detail.substr(0, 1100)}})); }
  catch (...) { client.channel->dropped.fetch_add(1, std::memory_order_relaxed); }
}
uint64_t BeginOperation(std::string_view operation, std::string_view detail) noexcept {
  static std::atomic<uint64_t> counter{0};
  const auto id = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  Event(operation, "begin", detail, id); return id;
}
void Progress(Role role) noexcept {
  if (!client.channel) return;
  const auto i = size_t(role);
  if (i >= client.channel->progress.size()) return;
  client.channel->progress[i].store(Now(), std::memory_order_relaxed);
  client.channel->counts[i].fetch_add(1, std::memory_order_relaxed);
}
void Active(Role role, bool active) noexcept {
  if (!client.channel || size_t(role) >= size_t(Role::kCount)) return;
  const auto i = size_t(role);
  if (active) {
    if (client.channel->active[i].fetch_add(1, std::memory_order_acq_rel) == 0)
      client.channel->active_since[i].store(Now(), std::memory_order_release);
  } else client.channel->active[i].fetch_sub(1, std::memory_order_release);
}
void FirstFailure(std::string_view operation, std::string_view evidence) noexcept {
  if (!client.channel) return;
  // Dedicated slot and stack-only encoding: publication is safe even while
  // the renderer holds its failure mutex or the process heap is compromised.
  auto& slot = client.channel->first_failure;
  uint64_t empty = 0;
  if (!slot.version.compare_exchange_strong(empty, 1, std::memory_order_acq_rel)) return;
  char value[kPayload]{}; size_t size = 0;
  auto literal = [&](std::string_view text) { for (char c : text) value[size++] = c; };
  auto quoted = [&](std::string_view text, size_t budget) {
    literal("\""); size_t used = 0;
    for (unsigned char c : text) {
      const size_t need = c < 32 ? 6 : c == '"' || c == '\\' ? 2 : 1;
      if (used + need > budget) { client.channel->truncated.fetch_add(1, std::memory_order_relaxed); break; }
      if (c < 32) {
        constexpr char hex[] = "0123456789abcdef";
        literal("\\u00"); value[size++] = hex[c >> 4]; value[size++] = hex[c & 15];
      } else { if (c == '"' || c == '\\') value[size++] = '\\'; value[size++] = char(c); }
      used += need;
    }
    literal("\"");
  };
  literal("{\"operation\":"); quoted(operation, 250); literal(",\"evidence\":"); quoted(evidence, 1600); literal("}");
  slot.time.store(Now(), std::memory_order_relaxed); slot.thread.store(ThreadId(), std::memory_order_relaxed);
  for (size_t i = 0; i < size; ++i) slot.bytes[i].store(value[i], std::memory_order_relaxed);
  slot.bytes[size].store(0, std::memory_order_relaxed);
  slot.version.store(2, std::memory_order_release);
}
void ExpectedExit(std::string_view reason) noexcept {
  Event("process-exit", "requested", reason);
  if (client.channel) client.channel->expected_exit.store(1, std::memory_order_release);
}
void Collector(bool ready, std::string_view reason) noexcept {
  Event("collector", ready ? "ready" : "degraded", reason);
  if (client.channel) client.channel->collector.store(ready ? 1 : 2, std::memory_order_release);
}
bool CollectorReady() noexcept {
  return client.channel && client.channel->collector.load(std::memory_order_acquire) == 1;
}
void SetMetadata(std::string_view key, std::string_view value) noexcept { Event(key, "metadata", value); }
void SetLog(const fs::path& path) noexcept {
  try { AtomicText(directory / "log-path.txt", Utf8(path)); Event("session-log", "attached", Utf8(path)); } catch (...) {}
}
void RequestSnapshot() noexcept { if (client.channel) client.channel->snapshot.fetch_add(1, std::memory_order_release); }
fs::path Directory() { return directory; }
fs::path ReportsDirectory() { return directory.parent_path(); }
std::string SessionId() { return Utf8(directory.filename()); }
fs::path Latest(bool failures_only) {
  try {
    fs::path latest;
    const auto root = ReportsDirectory();
    if (root.empty() || !fs::is_directory(root)) return {};
    for (const auto& entry : fs::directory_iterator(root)) {
      if (entry.is_symlink() || !entry.is_directory() || entry.path() == directory) continue;
      const auto manifest = Json::parse(ReadText(entry.path() / "manifest.json", 1024 * 1024), nullptr, false);
      if (manifest.is_discarded() || manifest.value("state", "") != "complete") continue;
      if (failures_only && manifest.value("classification", "") == "normal-exit") continue;
      if (latest.empty() || entry.path().filename() > latest.filename()) latest = entry.path();
    }
    if (failures_only && !latest.empty()) {
      const auto seen = ReadText(root / "acknowledged.txt", 256);
      if (!seen.empty() && Utf8(latest.filename()) <= seen) return {};
    }
    return latest;
  } catch (...) { return {}; }
}
void Acknowledge(const fs::path& session) noexcept {
  try { AtomicText(ReportsDirectory() / "acknowledged.txt", Utf8(session.filename())); } catch (...) {}
}
void GuestFault(uint64_t pc, uint64_t address, uint32_t kind, uint32_t thread,
                uint64_t base, const std::array<uint64_t, 32>& gpr, uint64_t lr, uint64_t ctr, uint32_t access_operation) noexcept {
  if (!client.channel) return;
  // Fixed-width hex encoding into stack storage: no formatter, heap, mutex,
  // guest memory walk or logger on the exception path.
  char text[1024]{};
  size_t used = 0;
  auto word = [&](uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    for (int n = 15; n >= 0; --n) text[used++] = digits[(value >> (n * 4)) & 15];
    text[used++] = ' ';
  };
  word(pc); word(address); word(kind); word(thread); word(base); word(lr); word(ctr);
  for (auto value : gpr) word(value);
  word(access_operation);
  auto& slot = client.channel->guest_fault;
  static std::atomic<uint64_t> observations{0};
  // This is a first-chance SDK observation, not proof of fatality. Refresh it
  // for later faults instead of allowing a recovered exception to consume the
  // final snapshot. Crashpad independently owns the original fatal CPU context.
  const auto version = (observations.fetch_add(1, std::memory_order_relaxed) + 1) * 2;
  if (!WriteSlot(slot, version, {text, used})) client.channel->dropped.fetch_add(1, std::memory_order_relaxed);
}
std::string ReadText(const fs::path& path, size_t limit) {
  std::ifstream file(path, std::ios::binary); std::string out(limit, '\0');
  file.read(out.data(), std::streamsize(limit)); out.resize(size_t(file.gcount())); return out;
}
bool AtomicText(const fs::path& path, std::string_view bytes) {
  auto temp = path; temp += ".tmp";
  { std::ofstream out(temp, std::ios::binary | std::ios::trunc); out.write(bytes.data(), std::streamsize(bytes.size())); out.close(); if (!out) return false; }
#ifdef _WIN32
  return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code error; fs::rename(temp, path, error); return !error;
#endif
}

// Defined in supervisor.cpp. Only the child attaches the global publication
// channel; the parent owns the mapping and persists its observations.
bool AttachClient(const fs::path& path) {
  directory = path;
  if (!client.Open(path / "channel.bin", false)) return false;
  client.channel->child_pid.store(Pid(), std::memory_order_release);
  Event("bootstrap", "child-ready"); return true;
}
}  // namespace narutorise::incident
