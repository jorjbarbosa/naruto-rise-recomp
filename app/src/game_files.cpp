// narutorise - ReXGlue Recompiled Project
//
// The game files against the manifest of the disc (game_files.h).

#include "game_files.h"

#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace narutorise::game_files {
namespace {

// The files of the disc, from tests/game_files_test.cpp manifest <root>.
constexpr File kManifest[] = {
#include "game_files_manifest.inc"
};
constexpr size_t kChunk = size_t(4) << 20;

std::filesystem::path RootPath(const std::filesystem::path& root) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(root, ec)) return root;
  std::string name = rex::path_to_utf8(root.filename());
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return name == "default.xex" ? root.parent_path() : root;
}

// The root's files through the device the runtime mounts for it; null when
// it cannot be read.
std::unique_ptr<rex::filesystem::Device> Mount(const std::filesystem::path& root) {
  const auto path = RootPath(root);
  std::unique_ptr<rex::filesystem::Device> device;
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec))
    device = std::make_unique<rex::filesystem::HostPathDevice>("\\Device\\GameFiles", path, true);
  else if (std::filesystem::is_regular_file(path, ec))
    device = std::make_unique<rex::filesystem::DiscImageDevice>("\\Device\\GameFiles", path);
  return device && device->Initialize() ? std::move(device) : nullptr;
}

rex::filesystem::Entry* FileEntry(rex::filesystem::Device& device, std::string_view path) {
  auto* entry = device.ResolvePath(path);
  return entry && !(entry->attributes() & rex::filesystem::kFileAttributeDirectory) ? entry : nullptr;
}

struct StateDeleter {
  void operator()(XXH3_state_t* state) const { XXH3_freeState(state); }
};

// The digest of an entry's data as rex::hash_bytes writes it; empty when a
// read fails or stop is set.
std::string Digest(rex::filesystem::Entry& entry, std::vector<uint8_t>& chunk, const std::atomic<bool>& stop) {
  std::unique_ptr<XXH3_state_t, StateDeleter> state(XXH3_createState());
  if (!state || XXH3_128bits_reset(state.get()) == XXH_ERROR) return {};
  rex::filesystem::File* file = nullptr;
  if (XFAILED(entry.Open(rex::filesystem::FileAccess::kFileReadData, &file)) || !file) return {};
  bool ok = true;
  for (size_t offset = 0; ok && offset < entry.size();) {
    size_t read = 0;
    const size_t wanted = std::min(chunk.size(), entry.size() - offset);
    ok = !stop.load(std::memory_order_relaxed) &&
         XSUCCEEDED(file->ReadSync(std::span(chunk.data(), wanted), offset, &read)) && read &&
         XXH3_128bits_update(state.get(), chunk.data(), read) != XXH_ERROR;
    offset += read;
  }
  file->Destroy();
  if (!ok) return {};
  const auto digest = XXH3_128bits_digest(state.get());
  return fmt::format("{:016x}{:016x}", digest.high64, digest.low64);
}

std::string ManifestDigest(std::span<const File> manifest) {
  std::string text;
  for (const auto& file : manifest) text += fmt::format("{}|{}|{}\n", file.path, file.size, file.xxh3);
  return rex::hash_bytes(text);
}

// The check runs beside the game's loading: below its threads, and on
// Windows with the background mode's low I/O priority too.
void LowerPriority() {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#elif defined(__linux__)
  const sched_param param{};
  pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
#endif
}

std::optional<Report> Load(const std::filesystem::path& cache, const std::string& fingerprint) {
  if (cache.empty()) return std::nullopt;
  std::ifstream in(cache, std::ios::binary);
  if (!in) return std::nullopt;
  const auto json = nlohmann::json::parse(in, nullptr, false);
  if (!json.is_object()) return std::nullopt;
  try {  // A value of another type: checked again.
    if (json.value("fingerprint", "") != fingerprint) return std::nullopt;
    Report report;
    const std::string verdict = json.value("verdict", "");
    for (const auto value : {Verdict::kOk, Verdict::kDamaged, Verdict::kOtherCopy})
      if (verdict == VerdictName(value)) report.verdict = value;
    if (report.verdict == Verdict::kUnchecked) return std::nullopt;
    report.checked = json.value("checked", 0u);
    report.damaged = json.value("damaged", std::vector<std::string>{});
    report.resized = json.value("resized", std::vector<std::string>{});
    report.missing = json.value("missing", std::vector<std::string>{});
    report.seconds = json.value("seconds", 0.0);
    report.notified = json.value("notified", false);
    return report;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

void Log(const Report& report, bool cached) {
  const auto files = report.Different();
  std::string names;
  for (size_t i = 0; i < files.size() && i < 8; ++i) names += (i ? ", " : "") + files[i];
  if (files.size() > 8) names += fmt::format(" and {} more", files.size() - 8);
  const auto line = fmt::format("[narutorise] Game files: {} ({} files, {} changed, {} of another size, {} missing; {})"
                                "{}{}", VerdictName(report.verdict), report.checked, report.damaged.size(),
                                report.resized.size(), report.missing.size(),
                                cached ? "checked before" : fmt::format("{:.1f} s", report.seconds),
                                names.empty() ? "" : ": ", names);
  if (report.verdict == Verdict::kOk)
    REXLOG_INFO("{}", line);
  else
    REXLOG_WARN("{}", line);
}

}  // namespace

std::vector<std::string> Report::Different() const {
  std::vector<std::string> files = damaged;
  files.insert(files.end(), resized.begin(), resized.end());
  files.insert(files.end(), missing.begin(), missing.end());
  return files;
}

std::span<const File> Manifest() { return kManifest; }

Report Verify(const std::filesystem::path& root, const std::atomic<bool>& stop) {
  return Verify(root, Manifest(), stop);
}

Report Verify(const std::filesystem::path& root, std::span<const File> manifest, const std::atomic<bool>& stop) {
  const auto start = std::chrono::steady_clock::now();
  Report report;
  const auto mounted = Mount(root);
  if (!mounted) return report;
  std::vector<uint8_t> chunk(kChunk);
  for (const auto& file : manifest) {
    if (stop.load()) return {};
    const std::string path(file.path);
    auto* entry = FileEntry(*mounted, path);
    ++report.checked;
    if (!entry) {
      report.missing.push_back(path);
    } else if (entry->size() != file.size) {
      report.resized.push_back(path);
    } else if (Digest(*entry, chunk, stop) != file.xxh3) {
      if (stop.load()) return {};
      report.damaged.push_back(path);  // An unreadable file too.
    }
  }
  // Missing files are a bad copy too; another release changes the sizes.
  report.verdict = report.Different().empty()                  ? Verdict::kOk
                   : report.resized.size() * 2 >= manifest.size() ? Verdict::kOtherCopy
                                                                  : Verdict::kDamaged;
  report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return report;
}

std::string Fingerprint(const std::filesystem::path& root, std::span<const File> manifest) {
  const auto path = RootPath(root);
  std::string text = ManifestDigest(manifest) + "\n" + rex::path_to_utf8(path) + "\n";
  std::error_code ec;
  if (std::filesystem::is_regular_file(path, ec)) {
    const auto size = std::filesystem::file_size(path, ec);
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return {};
    text += fmt::format("{} {}\n", size, time.time_since_epoch().count());
  } else {
    const auto mounted = Mount(path);
    if (!mounted) return {};
    for (const auto& file : manifest) {
      const auto* entry = FileEntry(*mounted, file.path);
      text += entry ? fmt::format("{} {}\n", entry->size(), entry->write_timestamp()) : "-\n";
    }
  }
  return rex::hash_bytes(text);
}

const char* VerdictName(Verdict verdict) {
  switch (verdict) {
    case Verdict::kOk:
      return "ok";
    case Verdict::kDamaged:
      return "damaged";
    case Verdict::kOtherCopy:
      return "other_copy";
    case Verdict::kUnchecked:
      return "unchecked";
  }
  return "unchecked";
}

std::string Tag(const Report& report) {
  if (report.verdict == Verdict::kDamaged) return fmt::format("damaged:{}", report.Different().size());
  return VerdictName(report.verdict);
}

Checker::~Checker() { Stop(); }

void Checker::Stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  stop_ = false;
}

void Checker::Start(std::filesystem::path root, std::filesystem::path cache, bool force,
                    std::function<void(const Report&)> done) {
  Stop();
  {
    std::lock_guard lock(mutex_);
    report_.reset();
    fingerprint_.clear();
  }
  root_ = std::move(root);
  cache_ = std::move(cache);
  running_ = true;
  thread_ = std::thread([this, force, done = std::move(done)] {
    LowerPriority();
    const auto fingerprint = Fingerprint(root_, manifest_);
    std::optional<Report> report;
    if (!force && !fingerprint.empty()) report = Load(cache_, fingerprint);
    const bool cached = report.has_value();
    if (!report) {
      report = Verify(root_, manifest_, stop_);
      // A forced check is the player's own: its result shows in the menu.
      report->notified = force;
      if (report->verdict != Verdict::kUnchecked && !fingerprint.empty()) Save(fingerprint, *report);
    }
    if (report->verdict != Verdict::kUnchecked || !stop_.load()) Log(*report, cached);
    if (done) done(*report);
    {
      std::lock_guard lock(mutex_);
      fingerprint_ = fingerprint;
      report_ = std::move(report);
    }
    ++generation_;
    running_ = false;
  });
}

std::optional<Report> Checker::Result() const {
  std::lock_guard lock(mutex_);
  return report_;
}

void Checker::MarkNotified() {
  std::lock_guard lock(mutex_);
  if (!report_ || report_->notified || fingerprint_.empty()) return;
  report_->notified = true;
  Save(fingerprint_, *report_);
}

void Checker::Save(const std::string& fingerprint, const Report& report) const {
  nlohmann::json json = {{"fingerprint", fingerprint}, {"verdict", VerdictName(report.verdict)},
                         {"checked", report.checked}, {"damaged", report.damaged}, {"resized", report.resized},
                         {"missing", report.missing}, {"seconds", report.seconds}, {"notified", report.notified}};
  if (cache_.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(cache_.parent_path(), ec);
  const auto temporary = std::filesystem::path(cache_).concat(".tmp");
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << json.dump(2) << '\n';
    if (!out) return;
  }
  std::filesystem::rename(temporary, cache_, ec);
  if (ec) REXLOG_WARN("[narutorise] Game files: the result was not saved ({})", ec.message());
}

}  // namespace narutorise::game_files
