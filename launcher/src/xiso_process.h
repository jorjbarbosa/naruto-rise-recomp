#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// Thin wrapper around the bundled extract-xiso.exe (XboxDev/extract-xiso).
// Runs it in list (-l) or extract (-x -s -d <dir>) mode, capturing its stdout
// to drive progress UI. Blocking: call from a worker thread and poll the
// shared Progress from the UI thread.
namespace xiso {

// Progress shared between the UI thread and the worker thread.
struct Progress {
  std::atomic<bool> cancel{false};       // ask to terminate the child process
  std::atomic<uint32_t> files_done{0};  // files listed/extracted so far

  void SetCurrentFile(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex);
    current_file = name;
  }
  std::string GetCurrentFile() const {
    std::lock_guard<std::mutex> lock(mutex);
    return current_file;
  }
  void AddLogLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex);
    lines.push_back(line);
    if (lines.size() > 64) {
      lines.erase(lines.begin(), lines.end() - 64);
    }
  }
  std::vector<std::string> GetRecentLines() const {
    std::lock_guard<std::mutex> lock(mutex);
    return lines;
  }

 private:
  mutable std::mutex mutex;
  std::string current_file;
  std::vector<std::string> lines;
};

struct RunResult {
  bool ok = false;
  bool canceled = false;
  int exit_code = -1;
  uint32_t files_total = 0;   // parsed from the final "N files ... total M bytes"
  uint64_t total_bytes = 0;   // parsed from the same line
  std::string output;          // full captured stdout/stderr
  std::string error;
};

// extract-xiso -l <iso>  (fast validation: lists the image without extracting).
RunResult RunList(const std::filesystem::path& extract_xiso_exe,
                  const std::filesystem::path& iso_path,
                  Progress& progress);

// extract-xiso -x -s -d <dest_dir> <iso>  (full extraction into dest_dir;
// skips the $SystemUpdate folder, like the reference installer).
RunResult RunExtract(const std::filesystem::path& extract_xiso_exe,
                     const std::filesystem::path& iso_path,
                     const std::filesystem::path& dest_dir,
                     Progress& progress);

// True when the captured output shows a default.xex in the image.
bool OutputHasDefaultXex(const std::string& output);

}  // namespace xiso
