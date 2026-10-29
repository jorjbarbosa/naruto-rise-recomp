#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace narutorise::incident {
constexpr uint32_t kChannelVersion = 1;
constexpr uint32_t kSlots = 256, kPayload = 2048;
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<char>::is_always_lock_free);
struct Slot {
  // Odd while writing. Both payload and version are atomic: a concurrent
  // overwrite is defined behavior, and a reader discards an inconsistent copy.
  std::atomic<uint64_t> version{0};
  std::atomic<uint64_t> time{0}, thread{0};
  std::array<std::atomic<char>, kPayload> bytes{};
};
struct Channel {
  uint32_t version = kChannelVersion, parent_pid = 0;
  std::atomic<uint64_t> next{0}, dropped{0}, truncated{0};
  std::atomic<uint32_t> child_pid{0}, expected_exit{0}, collector{0}, snapshot{0};
  std::array<std::atomic<uint64_t>, 5> progress{}, counts{};
  std::array<std::atomic<uint64_t>, 5> active_since{};
  std::array<std::atomic<uint32_t>, 5> active{};
  std::array<Slot, kSlots> events;
  Slot first_failure, guest_fault;
};
struct Mapping {
  Channel* channel = nullptr;
  void* file = nullptr;
  void* mapping = nullptr;
  int fd = -1;
  bool Open(const std::filesystem::path& path, bool create);
  ~Mapping();
};
uint64_t Now() noexcept;
uint32_t Pid() noexcept;
uint64_t ThreadId() noexcept;
std::string Utf8(const std::filesystem::path& path);
bool ReadSlot(const Slot& slot, uint64_t& version, uint64_t& time, uint64_t& thread, std::string& text);
bool WriteSlot(Slot& slot, uint64_t version, std::string_view text) noexcept;
std::string ReadText(const std::filesystem::path& path, size_t limit = 65536);
bool AtomicText(const std::filesystem::path& path, std::string_view bytes);
int Observe(Mapping& mapping, const std::filesystem::path& directory,
            const std::filesystem::path& executable, const std::vector<std::string>& arguments);
}  // namespace narutorise::incident
