#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <vector>

namespace gpu {
struct DrawStorageStats {
  uint64_t upstream_allocations = 0, retained_bytes = 0;
};
namespace detail {
// Count pool chunks, not individual draws. Large exceptional payloads bypass
// the pool; ordinary constant blocks and capture objects use size classes.
class DrawStorageUpstream final : public std::pmr::memory_resource {
 public:
  std::atomic<uint64_t> allocations{0}, bytes{0};
 private:
  void* do_allocate(size_t size, size_t alignment) override {
    void* p = std::pmr::new_delete_resource()->allocate(size, alignment);
    allocations.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(size, std::memory_order_relaxed);
    return p;
  }
  void do_deallocate(void* p, size_t size, size_t alignment) override {
    std::pmr::new_delete_resource()->deallocate(p, size, alignment);
    bytes.fetch_sub(size, std::memory_order_relaxed);
  }
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};
struct DrawStorageState {
  DrawStorageUpstream upstream;
  // A chunk must cover ordinary bursts; MSVC retains only one empty chunk
  // per size class, so small chunks caused new upstream allocations per burst.
  std::pmr::synchronized_pool_resource pool{std::pmr::pool_options{256, 8192}, &upstream};
};
inline DrawStorageState& DrawStorage() {
  static DrawStorageState state;
  return state;
}
}  // namespace detail

inline DrawStorageStats GetDrawStorageStats() {
  auto& s = detail::DrawStorage();
  return {s.upstream.allocations.load(std::memory_order_relaxed), s.upstream.bytes.load(std::memory_order_relaxed)};
}

// All allocators compare equal, so moving constants from capture to request
// transfers their storage even across threads. A block returns to the pool
// only when its vector is destroyed/reallocated, after its last CPU reader.
template <class T> struct DrawAllocator {
  using value_type = T;
  using is_always_equal = std::true_type;
  DrawAllocator() { (void)detail::DrawStorage(); }  // Pool outlives global constant carries too.
  template <class U> DrawAllocator(const DrawAllocator<U>&) noexcept {}
  T* allocate(size_t count) {
    if (count > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_array_new_length();
    return static_cast<T*>(detail::DrawStorage().pool.allocate(count * sizeof(T), alignof(T)));
  }
  void deallocate(T* p, size_t count) noexcept {
    detail::DrawStorage().pool.deallocate(p, count * sizeof(T), alignof(T));
  }
  template <class U> bool operator==(const DrawAllocator<U>&) const noexcept { return true; }
};
using ConstantBytes = std::vector<uint8_t, DrawAllocator<uint8_t>>;

// Class-specific allocation also works through StageTask's virtual destructor.
// The capture itself can be recycled while its moved constant vectors remain
// owned by a queued DrawRequest; those blocks are separate allocations.
template <class T> struct DrawStorageAllocated {
  static void* operator new(size_t bytes) { return detail::DrawStorage().pool.allocate(bytes, alignof(T)); }
  static void operator delete(void* p) noexcept {
    detail::DrawStorage().pool.deallocate(p, sizeof(T), alignof(T));
  }
};
}  // namespace gpu
