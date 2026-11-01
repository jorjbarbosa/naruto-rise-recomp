// narutorise - ReXGlue Recompiled Project
//
// Whether the files of a game data root are the ones of the disc the port
// was made with: the size and XXH3-128 of each file of the disc (the
// manifest, game_files_manifest.inc) against the root's, read through the
// devices the runtime mounts (a disc image in place, or a folder). A file of
// the same size with another digest is damaged or modified, as a bad copy of
// the disc leaves it (a .bik changed this way makes the game crash in its
// video decoder); many files of another size are another release or a fan
// edit. game_image.h only identifies the default.xex.
//
// The check reads the whole game (4.3 GiB), so it runs once per copy, in the
// background (Checker), and its report is kept with the copy's fingerprint.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace narutorise::game_files {

enum class Verdict {
  kOk,         // Every file of the disc, unchanged.
  kDamaged,    // Some files changed or missing.
  kOtherCopy,  // Half of the files or more of another size: another release or a fan edit.
  kUnchecked,  // Not read: stopped, or the root could not be opened.
};

struct Report {
  Verdict verdict = Verdict::kUnchecked;
  uint32_t checked = 0;              // Files of the manifest looked at.
  std::vector<std::string> damaged;  // The same size, another digest.
  std::vector<std::string> resized;  // Another size.
  std::vector<std::string> missing;
  double seconds = 0;
  bool notified = false;  // The player has seen it in the game (Checker::MarkNotified).
  // The files that differ: the damaged, then the resized and the missing.
  std::vector<std::string> Different() const;
};

// A file of the disc: its path in the root, its size and its XXH3-128 (the
// hex of rex::hash_bytes).
struct File {
  std::string_view path;
  uint64_t size;
  std::string_view xxh3;
};
std::span<const File> Manifest();

// root: a disc image, a folder with default.xex, or that default.xex (as
// game_image::Identify takes it). Stops early with kUnchecked when stop is set.
Report Verify(const std::filesystem::path& root, const std::atomic<bool>& stop);
Report Verify(const std::filesystem::path& root, std::span<const File> manifest, const std::atomic<bool>& stop);

// The copy at root without reading it: the manifest, the root and the size
// and write time of the image or of each file of the folder. Empty when the
// root cannot be read.
std::string Fingerprint(const std::filesystem::path& root, std::span<const File> manifest);

const char* VerdictName(Verdict verdict);
// The crash report tag: ok, damaged:N, other_copy or unchecked.
std::string Tag(const Report& report);

// The check of the session's root on a background thread, its report kept in
// cache (a JSON file in the data folder) with the copy's fingerprint: a copy
// already checked is not read again. Start and the queries from any thread;
// the destructor stops and joins.
class Checker {
 public:
  Checker() = default;
  explicit Checker(std::span<const File> manifest) : manifest_(manifest) {}
  ~Checker();
  Checker(const Checker&) = delete;
  Checker& operator=(const Checker&) = delete;

  // A cached report of the same copy, unless force; otherwise a new check.
  // done gets the report on the check's thread. An empty cache keeps none.
  void Start(std::filesystem::path root, std::filesystem::path cache, bool force = false,
             std::function<void(const Report&)> done = {});
  bool Running() const { return running_.load(); }
  // The report once known; it changes with each Start.
  std::optional<Report> Result() const;
  uint64_t Generation() const { return generation_.load(); }  // Counts the reports.
  // The in-game notice of this copy has shown: kept in the cache.
  void MarkNotified();

 private:
  void Stop();
  void Save(const std::string& fingerprint, const Report& report) const;
  std::span<const File> manifest_ = Manifest();
  std::filesystem::path root_, cache_;
  std::string fingerprint_;  // Of the report's copy, under the mutex.
  std::thread thread_;
  std::atomic<bool> stop_{false}, running_{false};
  std::atomic<uint64_t> generation_{0};
  mutable std::mutex mutex_;
  std::optional<Report> report_;
};

}  // namespace narutorise::game_files
