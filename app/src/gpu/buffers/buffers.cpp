// Vertex and index buffers of the D3D boundary (buffers.h).
#include "gpu/buffers/buffers.h"
#include "gpu/textures/write_watch.h"
#include "gpu/d3d/hooks.h"
#include "gpu/common/guest_memory.h"
#include "gpu/common/hash.h"
#include "gpu/backend/dispatcher.h"
#include "gpu/host/services.h"
#include "gpu/diagnostics/profile.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmemory.h>

using namespace gpu;

namespace gpu {
// --- Buffers ---
// Vertex and index data on the GPU. The game's static geometry is born
// outside CreateVertexBuffer (the only callers of the Lock 8212D950 are
// 8212C7E8, the frame start 820EFFB8,
// 82101C00 and 8210B318), and its dynamic geometry lives in two 4 MB rings (a
// VB ring and an IB ring per index size, alternating by frame) locked whole at
// the frame start by 820EFFB8 and bump-allocated per draw by 82101C00 and
// 8210B318, written before each draw while locked: the Unlock is no event for
// it, its write ranges go to the device (+11816), and the title's fetch
// constant is the only stable description of a draw's buffer. So the identity
// is the fetch range itself:
//   - a vertex range: (physical address, bytes, endian) of the fetch constant;
//   - an index range: (physical address, bytes, index size, endian) of the
//     draw's range of the index buffer (the header's size word has two
//     encodings: the ring's fetch form, 8210B318's raw bytes).
// A range first seen is on probation: the draw reads what it needs inline,
// and its first kBufferHashBytes are hashed once per Swap (the ring fetch
// constants cover the 4 MB left in the ring: hashing the whole range would
// read gigabytes). Seen again at a later Swap with the same hash, its needed
// bytes (rounded to pages, grown on demand) are uploaded once (UploadBuffer, a
// buffer of the engine) under the write watch (write_watch.h); from then on
// the draw binds it, and a
// write to its pages re-reads them and re-uploads those whose content changed
// (an index range re-reads whole, for its max index). A range the title
// rewrites (the rings: each draw gets the same offset every frame, with new
// content) is demoted after kBufferDemoteChanges changes, or marked dynamic
// when its prefix hash changes between Swaps on probation: inline from then
// on. The entries unused for kBufferIdleSwaps
// leave at the Swap, a destroyed VB or IB header (D750) retires its entries,
// and the engine releases a buffer once its last use completed.
constexpr uint64_t kBufferIdleSwaps = 16, kBufferSweepEvery = 4;
// The high bits are folded into the low ones, which pick the bucket
// of the MSVC map (power-of-two buckets); aligned addresses and sizes left the
// low bits of the products almost constant.
struct BufferKeyHash {
  size_t operator()(const BufferKey& k) const {
    uint64_t h = (uint64_t(k.address) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(k.bytes) * 0xC2B2AE3D27D4EB4Full) ^
                 (uint64_t(k.endian | k.index << 2 | k.index32 << 3) << 56);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    return size_t(h ^ (h >> 29));
  }
};
struct BufferEntry {  // Render thread only, but for the registry's map.
  BufferKey key;
  uint64_t id = 0;  // Engine key once hosted.
  bool hosted = false;
  // Demoted: its content changed while hosted (kBufferDemoteChanges times), or
  // its prefix hash changed between Swaps on probation: inline from then on,
  // without probes (the rings: 82101C00 and 8210B318 give each draw the same
  // offset every frame, so their keys repeat with new content).
  bool dynamic = false;
  uint64_t hash = 0, hash_swap = 0, seen_swaps = 0, hash_changes = 0;  // Probation: the prefix hash, once per Swap.
  uint64_t version = 0, last_use_swap = 0, uses = 0, uploads = 0, changes = 0;
  // Hosted bytes: the needed bytes seen, rounded to pages and grown on demand
  // (the ring fetch constants cover the 4 MB left in the ring); an index range
  // is hosted whole.
  uint32_t extent = 0;
  uint32_t max_index = 0;             // An index range: over the whole range, at its last read.
  std::vector<uint32_t> page_counts;  // Hosted: the watch count of each page of the extent at its last read.
  std::vector<uint64_t> page_hashes;  // Hosted: the hash of the entry's bytes in each page (a change of content).
  uint64_t chunk_sum = 0, checked_epoch = 0;
};
constexpr uint64_t kBufferDemoteChanges = 2;
std::atomic<uint64_t> buffer_ids{0}, buffer_versions{0};
std::atomic<uint64_t> buffer_binds{0}, buffer_hosted_binds{0}, buffer_hash_reads{0}, buffer_hash_bytes{0},
    buffer_probation_reads{0}, buffer_probation_bytes{0}, buffer_promotions{0}, buffer_grows{0}, buffer_demotions{0},
    buffer_dynamic_marks{0}, buffer_uploads{0}, buffer_upload_bytes{0}, buffer_partial_uploads{0},
    buffer_refreshes{0}, buffer_unchanged_refreshes{0}, buffer_reads_failed{0}, buffer_hosted_live{0},
    buffer_ring_marks{0}, buffer_first_sight_hosts{0};
class BufferRegistry {
 public:
  // Bumped whenever an entry leaves the map (the render thread's
  // cache of FindBuffer is emptied then).
  uint64_t Generation() const { return generation_.load(std::memory_order_acquire); }
  std::shared_ptr<BufferEntry> FindOrCreate(const BufferKey& k) {
    std::lock_guard lock(mutex_);
    auto& e = entries_[k];
    if (!e) {
      e = std::make_shared<BufferEntry>();
      e->key = k;
      ++created_;
    }
    return e;
  }
  // A hosted buffer no entry binds any more: released at the next Sweep.
  void Retire(uint64_t id) {
    std::lock_guard lock(mutex_);
    graveyard_.push_back(id);
  }
  // D750 of a VB or IB header (any thread): the entries at its address leave
  // the registry; the hosted ones are released at the next Sweep.
  size_t ForgetAddress(uint32_t address) {
    std::lock_guard lock(mutex_);
    size_t n = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->first.address != address) {
        ++it;
        continue;
      }
      if (it->second->hosted) {
        graveyard_.push_back(it->second->id);
        buffer_hosted_live.fetch_sub(1);
      }
      it = entries_.erase(it);
      ++n;
    }
    if (n) generation_.fetch_add(1, std::memory_order_release);
    forgotten_ += n;
    return n;
  }
  // At each Swap (render thread, after the draws of the frame): the entries
  // unused for kBufferIdleSwaps leave, and one posted operation releases the
  // retired buffers (FIFO behind the draws; the engine retires one still in use).
  void Sweep(uint64_t swap) {
    std::vector<uint64_t> release;
    bool evicted_now = false;
    {
      std::lock_guard lock(mutex_);
      if (swap % kBufferSweepEvery == 0)
        for (auto it = entries_.begin(); it != entries_.end();) {
          if (it->second->last_use_swap + kBufferIdleSwaps >= swap) {
            ++it;
            continue;
          }
          if (it->second->hosted) {
            graveyard_.push_back(it->second->id);
            buffer_hosted_live.fetch_sub(1);
          }
          it = entries_.erase(it);
          ++evicted_;
          evicted_now = true;
        }
      if (evicted_now) generation_.fetch_add(1, std::memory_order_release);
      release.swap(graveyard_);
    }
    if (release.empty()) return;
    released_ += release.size();
    auto* dispatcher = host::ActiveDispatcher();
    if (!dispatcher) return;
    dispatcher->Post([ids = std::move(release)](Engine& engine, std::string& out) {
      for (uint64_t id : ids)
        if (!engine.ReleaseBuffer(id, out)) return false;
      out = fmt::format("{{\"released\":{}}}", ids.size());
      return true;
    });
  }
  std::string Summary() {
    std::lock_guard lock(mutex_);
    size_t hosted = 0, probation = 0, dynamic = 0;
    uint64_t hosted_bytes = 0;
    for (const auto& [k, e] : entries_) {
      if (e->hosted) {
        ++hosted;
        hosted_bytes += e->extent;
      } else if (e->dynamic) {
        ++dynamic;
      } else {
        ++probation;
      }
    }
    return fmt::format("\"entries\":{},\"hosted\":{},\"hosted_bytes\":{},\"probation\":{},\"dynamic\":{},\"created\":{},"
                       "\"forgotten\":{},\"evicted\":{},\"released\":{},\"graveyard\":{}", entries_.size(), hosted,
                       hosted_bytes, probation, dynamic, created_, forgotten_, evicted_, released_, graveyard_.size());
  }
 private:
  std::mutex mutex_;
  std::unordered_map<BufferKey, std::shared_ptr<BufferEntry>, BufferKeyHash> entries_;
  std::vector<uint64_t> graveyard_;  // Hosted entries gone, their buffers not yet released.
  uint64_t created_ = 0, forgotten_ = 0, evicted_ = 0, released_ = 0;
  std::atomic<uint64_t> generation_{0};
} buffer_registry;
// The entries of the ranges the draws bind, direct-mapped per thread in front
// of the registry's lock and map. An entry leaving the registry empties the
// cache (generation); a removal racing a bind leaves the same orphan the
// lookup under the lock could return.
std::shared_ptr<BufferEntry> FindBuffer(const BufferKey& k) {
  constexpr size_t kSlots = 1024;
  struct Slot {
    BufferKey key;
    std::shared_ptr<BufferEntry> entry;
  };
  thread_local std::vector<Slot> slots(kSlots);
  thread_local uint64_t generation = UINT64_MAX;
  const uint64_t current = buffer_registry.Generation();
  if (generation != current) {
    for (auto& s : slots) s.entry.reset();
    generation = current;
  }
  Slot& s = slots[BufferKeyHash{}(k) & (kSlots - 1)];
  if (!s.entry || !(s.key == k)) {
    s.key = k;
    s.entry = buffer_registry.FindOrCreate(k);
  }
  return s.entry;
}
// The bytes of a range (offset, n) read from guest physical memory and
// swapped by the key's endian, as the draw read them before.
bool ReadRange(uint8_t* base, const BufferKey& k, uint32_t at, uint32_t n, std::vector<uint8_t>& out) {
  out.resize(n);
  return n <= k.bytes && at <= k.bytes - n && ReadPhysical(base, k.address + at, n, out.data()) && SwapEndian(out, k.endian);
}
uint32_t MaxIndex(const std::vector<uint8_t>& data, bool index32) {
  const size_t n = index32 ? 4 : 2;
  uint32_t m = 0;
  for (size_t i = 0; i + n <= data.size(); i += n) {
    uint32_t v = 0;
    std::memcpy(&v, &data[i], n);
    m = std::max(m, v);
  }
  return m;
}
bool PostBufferUpload(BufferUpload u, std::string& reason) {
  uint64_t bytes = 0;
  for (const auto& [offset, data] : u.ranges) bytes += data.size();
  auto* dispatcher = host::ActiveDispatcher();
  if (!dispatcher || !dispatcher->Post([posted = std::move(u)](Engine& engine, std::string& out) {
        return engine.UploadBuffer(posted, out); })) {
    reason = "buffer-upload-unproved";
    return false;
  }
  ++buffer_uploads;
  buffer_upload_bytes += bytes;
  return true;
}
// The pages of an extent and the entry's bytes (offset, n) inside page p.
uint32_t BufferPages(uint32_t address, uint32_t extent) {
  return ((address + extent - 1) >> write_watch::kPageShift) - (address >> write_watch::kPageShift) + 1;
}
std::pair<uint32_t, uint32_t> PageSpan(const BufferKey& k, uint32_t extent, uint32_t p) {
  const uint64_t page_at = uint64_t((k.address >> write_watch::kPageShift) + p) << write_watch::kPageShift;
  const uint32_t begin = uint32_t(std::max<uint64_t>(page_at, k.address) - k.address);
  const uint32_t end = uint32_t(std::min<uint64_t>(page_at + 4096, uint64_t(k.address) + extent) - k.address);
  return {begin, end - begin};
}
void BufferPageCounts(const BufferKey& k, uint32_t extent, std::vector<uint32_t>& counts) {
  const uint32_t first = k.address >> write_watch::kPageShift;
  counts.resize(BufferPages(k.address, extent));
  for (uint32_t p = 0; p < counts.size(); ++p) counts[p] = write_watch::page_writes[first + p].load(std::memory_order_relaxed);
}
void BufferPageHashes(const BufferKey& k, uint32_t extent, const std::vector<uint8_t>& data, std::vector<uint64_t>& hashes) {
  hashes.resize(BufferPages(k.address, extent));
  for (uint32_t p = 0; p < hashes.size(); ++p) {
    const auto [offset, n] = PageSpan(k, extent, p);
    hashes[p] = FastHash64(data.data() + offset, n, n);
  }
}
// The hosted extent of a vertex range: the needed bytes rounded to pages, at
// least twice the current one when growing, never past the fetch range.
uint32_t HostedExtent(const BufferKey& k, uint32_t needed, uint32_t current) {
  if (k.index) return k.bytes;
  const uint64_t extent = std::max<uint64_t>((uint64_t(needed) + 4095) & ~uint64_t(4095), uint64_t(current) * 2);
  return uint32_t(std::min<uint64_t>(extent, k.bytes));
}
// Hosts the entry (or re-hosts it with a larger extent; the engine recreates
// the buffer when its size changes): the counts before the arm, the extent
// read after it, as UploadTexture.
bool HostEntry(uint8_t* base, BufferEntry& e, uint32_t extent, std::string& reason) {
  const BufferKey& k = e.key;
  const uint64_t epoch = write_watch::epoch.load(std::memory_order_acquire);
  const std::vector<std::pair<uint32_t, uint32_t>> range{{k.address, extent}};
  const uint64_t chunks = write_watch::Sum(range, false);
  std::vector<uint32_t> counts;
  BufferPageCounts(k, extent, counts);
  write_watch::Arm(range);
  std::vector<uint8_t> data;
  if (!ReadRange(base, k, 0, extent, data)) {
    ++buffer_reads_failed;
    reason = "buffer-range-unreadable";
    return false;
  }
  if (k.index) e.max_index = MaxIndex(data, k.index32);
  BufferPageHashes(k, extent, data, e.page_hashes);
  if (!e.hosted) {
    e.hosted = true;
    e.id = ++buffer_ids;
    ++buffer_promotions;
    ++buffer_hosted_live;
  } else {
    ++buffer_grows;
  }
  e.extent = extent;
  e.page_counts = std::move(counts);
  e.chunk_sum = chunks;
  e.checked_epoch = epoch;
  BufferUpload u;
  u.key = e.id;
  u.bytes = extent;
  u.version = ++buffer_versions;
  u.ranges.emplace_back(0u, std::move(data));
  e.version = u.version;
  if (!PostBufferUpload(std::move(u), reason)) return false;
  ++e.uploads;
  return true;
}
// A hosted range: when a write reached its chunks since the last read, the
// pages whose count changed are re-armed and re-read; a page whose hash holds
// (a write elsewhere in a shared page, or the same bytes again) is not
// uploaded; the others are, as runs (an index range whole, for its max index).
// The kBufferDemoteChanges-th change of content demotes the entry: it is
// dynamic, read inline from then on, and its buffer is retired.
bool RefreshHosted(uint8_t* base, BufferEntry& e, std::string& reason, bool& demoted) {
  demoted = false;
  const BufferKey& k = e.key;
  const uint32_t extent = e.extent;
  const uint64_t epoch = write_watch::epoch.load(std::memory_order_acquire);
  if (e.checked_epoch == epoch) return true;
  const std::vector<std::pair<uint32_t, uint32_t>> range{{k.address, extent}};
  const uint64_t chunks = write_watch::Sum(range, false);
  if (chunks == e.chunk_sum) {
    e.checked_epoch = epoch;
    return true;
  }
  std::vector<uint32_t> counts;
  BufferPageCounts(k, extent, counts);
  std::vector<uint32_t> dirty;
  for (uint32_t p = 0; p < counts.size(); ++p)
    if (counts[p] != e.page_counts[p]) dirty.push_back(p);
  ++buffer_refreshes;
  if (dirty.empty()) {  // The write hit the chunk outside this extent's pages.
    e.chunk_sum = chunks;
    e.checked_epoch = epoch;
    return true;
  }
  std::vector<std::pair<uint32_t, uint32_t>> arm;
  for (uint32_t p : dirty) {
    const auto [offset, n] = PageSpan(k, extent, p);
    arm.emplace_back(k.address + offset, n);
  }
  write_watch::Arm(arm);
  BufferUpload u;
  u.key = e.id;
  u.bytes = extent;
  bool changed = false;
  if (k.index) {
    std::vector<uint8_t> data;
    if (!ReadRange(base, k, 0, extent, data)) {
      ++buffer_reads_failed;
      reason = "buffer-range-unreadable";
      return false;
    }
    std::vector<uint64_t> hashes;
    BufferPageHashes(k, extent, data, hashes);
    changed = hashes != e.page_hashes;
    if (changed) {
      e.max_index = MaxIndex(data, k.index32);
      e.page_hashes = std::move(hashes);
      u.ranges.emplace_back(0u, std::move(data));
    }
  } else {
    for (uint32_t p : dirty) {
      const auto [offset, n] = PageSpan(k, extent, p);
      std::vector<uint8_t> data;
      if (!ReadRange(base, k, offset, n, data)) {
        ++buffer_reads_failed;
        reason = "buffer-range-unreadable";
        return false;
      }
      const uint64_t h = FastHash64(data.data(), n, n);
      if (h == e.page_hashes[p]) continue;
      e.page_hashes[p] = h;
      changed = true;
      if (!u.ranges.empty() && u.ranges.back().first + u.ranges.back().second.size() == offset)
        u.ranges.back().second.insert(u.ranges.back().second.end(), data.begin(), data.end());
      else
        u.ranges.emplace_back(offset, std::move(data));
    }
  }
  e.page_counts = std::move(counts);
  e.chunk_sum = chunks;
  e.checked_epoch = epoch;
  if (!changed) {
    ++buffer_unchanged_refreshes;
    return true;
  }
  if (++e.changes >= kBufferDemoteChanges) {
    buffer_registry.Retire(e.id);
    e.hosted = false;
    e.dynamic = true;
    e.extent = 0;
    e.page_counts.clear();
    e.page_hashes.clear();
    ++buffer_demotions;
    buffer_hosted_live.fetch_sub(1);
    demoted = true;
    return true;
  }
  u.version = ++buffer_versions;
  const bool partial = u.ranges.size() != 1 || u.ranges[0].second.size() != extent;
  e.version = u.version;
  if (!PostBufferUpload(std::move(u), reason)) return false;
  ++e.uploads;
  if (partial) ++buffer_partial_uploads;
  return true;
}
// The dynamic rings of the Jade engine, which 820EFFB8 locks whole at the start
// of each frame: per parity p, the 60-byte block at 0x82C218C8 + 60p holds the
// VB ring header (+0: data at +0x18, bytes in bits 2..25 of +0x1C, the fetch
// form) and the two IB ring headers (+12, +16: data at +0x18, bytes at +0x1C).
// Read once per Swap; known once both VB rings exist.
struct RingRegions {
  uint64_t swap = UINT64_MAX;
  std::array<std::pair<uint32_t, uint32_t>, 6> ranges{};  // (physical address, bytes)
  bool known = false;
} ring_regions;  // Render thread only.
const RingRegions& Rings(uint8_t* base, uint64_t swap) {
  RingRegions& r = ring_regions;
  if (r.swap == swap) return r;
  r.swap = swap;
  r.ranges = {};
  unsigned n = 0, vb_rings = 0;
  for (uint32_t p = 0; p < 2; ++p) {
    const uint32_t block = 0x82C218C8u + 60 * p;
    const uint32_t vb = Get32(base, block);
    if (vb) {
      r.ranges[n] = {GuestPhysicalAlias(Get32(base, vb + 24) & ~3u), Get32(base, vb + 28) & 0x03FFFFFCu};
      vb_rings += r.ranges[n].second != 0;
    }
    ++n;
    for (const uint32_t at : {block + 12, block + 16}) {
      const uint32_t ib = Get32(base, at);
      if (ib) r.ranges[n] = {GuestPhysicalAlias(Get32(base, ib + 24)), Get32(base, ib + 28)};
      ++n;
    }
  }
  r.known = vb_rings == 2;
  return r;
}
bool InRing(const RingRegions& r, const BufferKey& k) {
  for (const auto& [at, bytes] : r.ranges)
    if (bytes && k.address < at + bytes && at < k.address + k.bytes) return true;
  return false;
}
bool BindBuffer(uint8_t* base, const BufferKey& k, uint32_t needed, uint64_t swap, BufferUse& use, std::string& reason) {
  ++buffer_binds;
  if (!k.bytes || k.bytes > kBufferBytesMax || needed > k.bytes) {
    reason = "buffer-range-size";
    return false;
  }
  const std::shared_ptr<BufferEntry> entry = FindBuffer(k);
  BufferEntry& e = *entry;
  e.last_use_swap = swap;
  ++e.uses;
  auto bind_hosted = [&] {
    ++buffer_hosted_binds;
    use.hosted = true;
    use.key = e.id;
    use.offset = 0;
    use.bytes = e.extent;
    use.max_index = e.max_index;
    return true;
  };
  if (e.hosted) {
    bool demoted = false;
    if (!RefreshHosted(base, e, reason, demoted)) return false;
    if (!demoted) {
      if (needed > e.extent && !HostEntry(base, e, HostedExtent(k, needed, e.extent), reason)) return false;
      return bind_hosted();
    }
  }
  // At its first sight, a range inside a ring is dynamic (inline, no hash) and
  // any other is hosted at once: the static geometry the engine builds outside
  // CreateVertexBuffer, uploaded once (the probation would copy up to ~100 MB
  // inline in a frame where new geometry appears). The write watch and the
  // demotion keep a range the title rewrites correct; before the rings exist,
  // the probation applies.
  if (!e.hosted && !e.dynamic && !e.seen_swaps) {
    const RingRegions& rings = Rings(base, swap);
    if (rings.known) {
      if (InRing(rings, k)) {
        e.dynamic = true;
        ++buffer_ring_marks;
      } else {
        if (!HostEntry(base, e, HostedExtent(k, needed, 0), reason)) return false;
        ++buffer_first_sight_hosts;
        return bind_hosted();
      }
    }
  }
  if (!e.dynamic && e.hash_swap != swap) {  // Once per Swap: the hash of the range's prefix.
    const uint32_t probe = std::min(k.bytes, kBufferHashBytes);
    std::vector<uint8_t> data;
    if (!ReadRange(base, k, 0, probe, data)) {
      ++buffer_reads_failed;
      reason = "buffer-range-unreadable";
      return false;
    }
    ++buffer_hash_reads;
    buffer_hash_bytes += probe;
    const uint64_t hash = FastHash64(data.data(), data.size(), k.bytes);
    if (e.seen_swaps > 0 && hash == e.hash) {  // A later Swap, the same hash: promoted.
      if (!HostEntry(base, e, HostedExtent(k, needed, 0), reason)) return false;
      return bind_hosted();
    }
    if (e.seen_swaps > 0 && ++e.hash_changes >= kBufferDemoteChanges) {  // Rewritten between Swaps: dynamic.
      e.dynamic = true;
      ++buffer_dynamic_marks;
    }
    e.hash = hash;
    e.hash_swap = swap;
    ++e.seen_swaps;
    if (k.index && probe == k.bytes) {  // The whole range is the draw's, already read.
      use.max_index = MaxIndex(data, k.index32);
      use.data = std::move(data);
      use.bytes = k.bytes;
      ++buffer_probation_reads;
      buffer_probation_bytes += k.bytes;
      profile::Count(profile::kBufferInlineBytes, k.bytes);
      return true;
    }
  }
  if (!ReadRange(base, k, 0, needed, use.data)) {
    ++buffer_reads_failed;
    reason = "buffer-range-unreadable";
    return false;
  }
  if (k.index) use.max_index = MaxIndex(use.data, k.index32);
  use.bytes = needed;
  ++buffer_probation_reads;
  buffer_probation_bytes += needed;
  profile::Count(profile::kBufferInlineBytes, needed);
  return true;
}
void BufferRegistrySweep(uint64_t swap) { buffer_registry.Sweep(swap); }
// D750 of a header: a vertex buffer (D3DRESOURCETYPE 1) or an index buffer (2)
// retires the entries at its base (+0x18, low bits the fetch type).
size_t ForgetBufferHeader(uint8_t* base, uint32_t header) {
  const uint32_t type = Get32(base, header) & 0xF;
  if (type != 1 && type != 2) return 0;
  const uint32_t address = Get32(base, header + 0x18) & ~3u;
  if (!address) return 0;
  const uint32_t physical = GuestPhysicalAlias(address);
  // On the translation stage, after the captures that still bind the entries (the
  // count is not known to the title then).
  if (auto* dispatcher = host::ActiveDispatcher(); dispatcher && dispatcher->Deferred() && !dispatcher->OnTranslator()) {
    dispatcher->Defer([physical] { buffer_registry.ForgetAddress(physical); });
    return 0;
  }
  return buffer_registry.ForgetAddress(physical);
}
std::string BufferRegistrySummary() {
  return buffer_registry.Summary() + fmt::format(
      ",\"binds\":{},\"hosted_binds\":{},\"hosted_live\":{},\"promotions\":{},\"grows\":{},\"demotions\":{},"
      "\"dynamic_marks\":{},\"uploads\":{},\"upload_bytes\":{},\"partial_uploads\":{},\"refreshes\":{},"
      "\"unchanged_refreshes\":{},\"hash_reads\":{},\"hash_bytes\":{},\"probation_reads\":{},"
      "\"probation_bytes\":{},\"reads_failed\":{},\"ring_marks\":{},\"first_sight_hosts\":{},\"rings\":[{}]",
      buffer_binds.load(), buffer_hosted_binds.load(),
      buffer_hosted_live.load(), buffer_promotions.load(), buffer_grows.load(), buffer_demotions.load(),
      buffer_dynamic_marks.load(), buffer_uploads.load(), buffer_upload_bytes.load(), buffer_partial_uploads.load(),
      buffer_refreshes.load(), buffer_unchanged_refreshes.load(), buffer_hash_reads.load(), buffer_hash_bytes.load(),
      buffer_probation_reads.load(), buffer_probation_bytes.load(), buffer_reads_failed.load(),
      buffer_ring_marks.load(), buffer_first_sight_hosts.load(), [] {
        std::string out;
        for (const auto& [at, bytes] : ring_regions.ranges)
          out += fmt::format("{}[{},{}]", out.empty() ? "" : ",", at, bytes);
        return out;
      }());
}
}  // namespace gpu
