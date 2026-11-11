#pragma once

// Guest resources of the D3D boundary: the registry of guest D3D headers and
// their own resources, the content of texture headers (own resolves, CPU
// writes), and the coverage of the surface formats.
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fmt/format.h>

namespace gpu {
// Resource layer: guest D3D headers mapped to own resources with a
// generation per registration. Render targets and depth surfaces get a Plume
// surface at creation; textures keep their descriptor until first sampled.
class ResourceRegistry final {
 public:
  enum class Kind { Surface, Texture };
  struct Entry {
    Kind kind;
    uint64_t generation;
    bool host_surface;
    std::array<uint32_t, 8> args;
  };
  uint64_t Register(uint32_t key, Kind kind, bool host_surface, const std::array<uint32_t, 8>& args) {
    std::lock_guard lock(mutex_);
    const uint64_t generation = ++next_generation_;
    if (!entries_.emplace(key, Entry{kind, generation, host_surface, args}).second) return 0;
    ++created_[kind == Kind::Surface];
    if (host_surface) ++host_surfaces_;
    return generation;
  }
  bool Find(uint32_t key, Entry& entry) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    entry = it->second;
    return true;
  }
  // Removes the entry only for the generation the caller released.
  bool Take(uint32_t key, uint64_t generation, Entry& entry) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end() || it->second.generation != generation) return false;
    entry = it->second;
    entries_.erase(it);
    ++released_[entry.kind == Kind::Surface];
    if (entry.host_surface) --host_surfaces_;
    return true;
  }
  uint32_t Samples(uint32_t key) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return 0;
    const uint32_t ms = it->second.args[3];  // D3DMULTISAMPLE_TYPE argument of D330
    return ms == 2 ? 4 : ms == 1 ? 2 : 1;
  }
  size_t Live() {  // The live headers (textures and surfaces), sampled at each Swap.
    std::lock_guard lock(mutex_);
    return entries_.size();
  }
  std::vector<uint32_t> Keys(Kind kind) {
    std::lock_guard lock(mutex_);
    std::vector<uint32_t> keys;
    for (const auto& [key, entry] : entries_)
      if (entry.kind == kind) keys.push_back(key);
    return keys;
  }
  std::string Summary() {
    std::lock_guard lock(mutex_);
    return fmt::format("\"live\":{},\"textures_created\":{},\"surfaces_created\":{},"
        "\"textures_released\":{},\"surfaces_released\":{},\"live_host_surfaces\":{}", entries_.size(),
        created_[0], created_[1], released_[0], released_[1], host_surfaces_);
  }
 private:
  std::mutex mutex_;
  std::unordered_map<uint32_t, Entry> entries_;
  uint64_t next_generation_ = 0;
  std::array<uint64_t, 2> created_{}, released_{};
  uint64_t host_surfaces_ = 0;
};
extern ResourceRegistry resources;

// Texture headers whose content has an own copy (resolve
// destinations) and the CPU writes made to each header through the texture
// locks (records). The draw layer finds a resolve destination by the base
// address of its fetch constant (header +0x20), recorded at the resolve;
// its bindings are translated again when a resolve is noted or forgotten.
class TextureContent {
 public:
  // The address is the physical base of the destination (the key a draw's
  // fetch constant carries). The bindings are translated again only when the
  // set changes, not at every resolve of the frame.
  void NoteResolve(uint32_t key, uint32_t address) {
    std::lock_guard lock(mutex_);
    const bool inserted = resolved_.insert(key).second;
    auto it = resolved_at_.find(address);
    const bool moved = it == resolved_at_.end() || it->second != key;
    resolved_at_[address] = key;
    if (inserted || moved) resolve_generation_.fetch_add(1, std::memory_order_release);
  }
  void Forget(uint32_t key) {
    std::lock_guard lock(mutex_);
    ++destroyed_[key];  // Lifetime evidence of unregistered headers.
    if (resolved_.erase(key)) resolve_generation_.fetch_add(1, std::memory_order_release);
    writes_.erase(key);
    std::erase_if(resolved_at_, [key](const auto& e) { return e.second == key; });
  }
  uint64_t ResolveGeneration() const { return resolve_generation_.load(std::memory_order_acquire); }
  bool Resolved(uint32_t key) {
    std::lock_guard lock(mutex_);
    return resolved_.count(key) != 0;
  }
  uint64_t NoteCpuWrite(uint32_t key) {
    std::lock_guard lock(mutex_);
    ++locks_;
    return ++writes_[key];
  }
  uint64_t Writes(uint32_t key) {
    std::lock_guard lock(mutex_);
    auto it = writes_.find(key);
    return it == writes_.end() ? 0 : it->second;
  }
  // Header whose own resolve or CPU writes describe the memory at address; 0 if none.
  uint32_t ResolvedAt(uint32_t address) {
    std::lock_guard lock(mutex_);
    auto it = resolved_at_.find(address);
    return it == resolved_at_.end() ? 0 : it->second;
  }
  uint64_t Locks() {
    std::lock_guard lock(mutex_);
    return locks_;
  }
  uint64_t Destroyed(uint32_t key) {
    std::lock_guard lock(mutex_);
    auto it = destroyed_.find(key);
    return it == destroyed_.end() ? 0 : it->second;
  }
  std::string Summary() {
    std::lock_guard lock(mutex_);
    return fmt::format("\"texture_locks\":{},\"locked_headers\":{},\"resolved_headers\":{}", locks_,
                       writes_.size(), resolved_.size());
  }
 private:
  std::mutex mutex_;
  std::unordered_set<uint32_t> resolved_;
  std::unordered_map<uint32_t, uint64_t> writes_;
  std::unordered_map<uint32_t, uint32_t> resolved_at_;  // base address -> header
  std::unordered_map<uint32_t, uint64_t> destroyed_;  // header -> D750 calls
  uint64_t locks_ = 0;
  std::atomic<uint64_t> resolve_generation_{0};
};
extern TextureContent texture_content;
std::string ResourceSummary();
// The own resource of a registered guest header, released on the engine owner
// (D750 and the ordered close). False: unproved (evidence says why).
bool ReleaseRegistered(uint32_t key, std::string& evidence);
// A header that may have been a resolve destination without ever being
// registered (a texture header embedded in a game structure): the own texture
// the engine created for its resolves, released on the engine owner as
// ReleaseRegistered releases a registered one (which it does for those).
bool ReleaseResolveDestination(uint32_t key, std::string& evidence);
// The sampled copy of a texture header (its physical base), released as D750
// does; true for any other resource type. False: unproved (evidence says why).
bool ReleaseSampledCopy(uint8_t* base, uint32_t header, std::string& evidence);
// The first field of a color surface format outside the covered RGBA8, or null.
const char* UncoveredColorField(uint32_t format, uint32_t info);
bool DepthFormatCovered(uint32_t format);
}  // namespace gpu
