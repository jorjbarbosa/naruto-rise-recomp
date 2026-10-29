#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <mutex>
#include <new>
#include <type_traits>
#include <vector>

namespace gpu {
struct DrawStorageStats {
  uint64_t upstream_allocations = 0, retained_bytes = 0;
};
// The blocks of the pool: power-of-two size classes of 64 bytes to 16 KiB,
// 64-byte aligned. Larger or more aligned requests go to the upstream.
inline constexpr size_t kDrawStorageLargestBlock = 16384;
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
// Size-class pool with per-thread magazines: the capture allocates on the
// title thread and the translator and the owner free, every draw. Each thread
// takes and returns blocks on its own lists without a lock; whole magazines of
// kMagazine blocks move through the shared depot under its mutex, once per
// kMagazine blocks. Blocks are never given back to the upstream.
class DrawPool {
 public:
  explicit DrawPool(std::pmr::memory_resource& upstream) : upstream_(upstream) {}
  DrawPool(const DrawPool&) = delete;
  DrawPool& operator=(const DrawPool&) = delete;
  void* allocate(size_t bytes, size_t alignment) {
    const int c = Class(bytes, alignment);
    if (c < 0) return upstream_.allocate(bytes, alignment);
    Cache& cache = Local();
    Magazine& loaded = cache.loaded[c];
    if (!loaded.head) {
      Magazine& spare = cache.spare[c];
      if (spare.head) std::swap(loaded, spare);
      else loaded = Take(c);
    }
    Node* node = loaded.head;
    loaded.head = node->next;
    --loaded.count;
    return node;
  }
  void deallocate(void* p, size_t bytes, size_t alignment) noexcept {
    const int c = Class(bytes, alignment);
    if (c < 0) { upstream_.deallocate(p, bytes, alignment); return; }
    Cache& cache = Local();
    Magazine& loaded = cache.loaded[c];
    if (loaded.count == kMagazine) {
      Magazine& spare = cache.spare[c];
      if (spare.head) Give(c, spare);
      spare = loaded;
      loaded = {};
    }
    auto* node = static_cast<Node*>(p);
    node->next = loaded.head;
    loaded.head = node;
    ++loaded.count;
  }

 private:
  static constexpr unsigned kClasses = 9;  // 64 << 0 .. 64 << 8.
  static constexpr uint32_t kMagazine = 64;
  struct Node { Node* next; };
  struct Magazine { Node* head = nullptr; uint32_t count = 0; };
  // A thread's magazines; at its exit they go back to the depot.
  struct Cache {
    DrawPool* pool = nullptr;
    std::array<Magazine, kClasses> loaded{}, spare{};
    ~Cache() {
      if (!pool) return;
      for (unsigned c = 0; c < kClasses; ++c) {
        if (loaded[c].head) pool->Give(c, loaded[c]);
        if (spare[c].head) pool->Give(c, spare[c]);
      }
    }
  };
  static int Class(size_t bytes, size_t alignment) {
    if (alignment > 64 || bytes > kDrawStorageLargestBlock) return -1;
    return bytes <= 64 ? 0 : int(std::bit_width(bytes - 1)) - 6;
  }
  Cache& Local() {
    thread_local Cache cache;
    cache.pool = this;  // One pool per process (DrawStorage).
    return cache;
  }
  // A magazine from the depot, or a new chunk of kMagazine blocks.
  Magazine Take(unsigned c) {
    {
      std::lock_guard lock(mutex_);
      if (!depot_[c].empty()) {
        const Magazine m = depot_[c].back();
        depot_[c].pop_back();
        return m;
      }
    }
    const size_t size = size_t(64) << c;
    auto* chunk = static_cast<uint8_t*>(upstream_.allocate(size * kMagazine, 64));
    for (uint32_t i = 0; i < kMagazine; ++i)
      reinterpret_cast<Node*>(chunk + i * size)->next =
          i + 1 < kMagazine ? reinterpret_cast<Node*>(chunk + (i + 1) * size) : nullptr;
    return {reinterpret_cast<Node*>(chunk), kMagazine};
  }
  void Give(unsigned c, Magazine& m) noexcept {
    std::lock_guard lock(mutex_);
    depot_[c].push_back(m);
    m = {};
  }
  std::pmr::memory_resource& upstream_;
  std::mutex mutex_;
  std::array<std::vector<Magazine>, kClasses> depot_;
};
struct DrawStorageState {
  DrawStorageUpstream upstream;
  DrawPool pool{upstream};
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
