// Headless ISO bridge for Inno Setup. No UI or SDL dependency.
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "xiso_process.h"

namespace {
namespace fs = std::filesystem;
constexpr uint32_t kTitleId = 0x555307E5;  // Naruto: Rise of a Ninja

uint32_t Big32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
         uint32_t(p[2]) << 8 | p[3];
}
uint32_t Little32(const uint8_t* p) {
  return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 |
         uint32_t(p[1]) << 8 | p[0];
}
void ReadAt(std::ifstream& in, uint64_t offset, void* data, size_t size,
            uint64_t limit) {
  if (offset > limit || size > limit - offset) throw std::runtime_error("INVALID_ISO");
  in.clear();
  in.seekg(static_cast<std::streamoff>(offset));
  in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
  if (!in) throw std::runtime_error("INVALID_ISO");
}

void ValidateXex(std::ifstream& in, uint64_t base, uint64_t size) {
  std::array<uint8_t, 24> header{};
  ReadAt(in, base, header.data(), header.size(), base + size);
  if (std::memcmp(header.data(), "XEX2", 4) != 0) throw std::runtime_error("WRONG_GAME");
  const uint32_t count = Big32(header.data() + 20);
  if (count > 4096 || 24ull + count * 8ull > size) throw std::runtime_error("WRONG_GAME");
  for (uint32_t i = 0; i < count; ++i) {
    std::array<uint8_t, 8> entry{};
    ReadAt(in, base + 24 + i * 8ull, entry.data(), entry.size(), base + size);
    if (Big32(entry.data()) == 0x00040006) {  // XEX execution information
      std::array<uint8_t, 24> execution{};
      ReadAt(in, base + Big32(entry.data() + 4), execution.data(), execution.size(), base + size);
      if (Big32(execution.data() + 12) != kTitleId) throw std::runtime_error("WRONG_GAME");
      return;
    }
  }
  throw std::runtime_error("WRONG_GAME");
}

// Read only the volume, root directory and XEX optional headers. Full disc and
// trimmed XISO offsets match extract-xiso's supported XDVDFS layouts.
void ValidateIso(const fs::path& iso) {
  std::ifstream in(iso, std::ios::binary);
  const uint64_t size = fs::file_size(iso);
  for (uint64_t partition : {0ull, 0x0FD90000ull, 0x02080000ull, 0x18300000ull}) {
    if (partition + 0x10800 > size) continue;
    std::array<uint8_t, 2048> volume{};
    ReadAt(in, partition + 0x10000, volume.data(), volume.size(), size);
    if (std::memcmp(volume.data(), "MICROSOFT*XBOX*MEDIA", 20) != 0) continue;
    if (std::memcmp(volume.data() + 2028, "MICROSOFT*XBOX*MEDIA", 20) != 0)
      throw std::runtime_error("INVALID_ISO");
    const uint64_t root = partition + Little32(volume.data() + 20) * 2048ull;
    const uint32_t root_size = Little32(volume.data() + 24);
    if (root_size < 14 || root_size > 16 * 1024 * 1024)
      throw std::runtime_error("INVALID_ISO");
    std::vector<uint8_t> table(root_size);
    ReadAt(in, root, table.data(), table.size(), size);
    for (size_t pos = 0; pos + 14 <= table.size();) {
      const uint8_t* entry = table.data() + pos;
      if (entry[0] == 0xFF && entry[1] == 0xFF) {
        pos = (pos / 2048 + 1) * 2048;
        continue;
      }
      const size_t length = entry[13];
      if (!length || pos + 14 + length > table.size()) throw std::runtime_error("INVALID_ISO");
      std::string name(reinterpret_cast<const char*>(entry + 14), length);
      if (!(entry[12] & 0x10) && _stricmp(name.c_str(), "default.xex") == 0) {
        const uint64_t start = partition + Little32(entry + 4) * 2048ull;
        const uint64_t xex_size = Little32(entry + 8);
        if (start > size || xex_size > size - start) throw std::runtime_error("INVALID_ISO");
        ValidateXex(in, start, xex_size);
        return;
      }
      pos += (14 + length + 3) & ~size_t(3);
    }
    throw std::runtime_error("WRONG_GAME");
  }
  throw std::runtime_error("INVALID_ISO");
}

int Run(const std::wstring& mode, const fs::path& iso, const fs::path& dest,
        const fs::path& cancel, uint32_t total) {
  ValidateIso(iso);
  if (fs::exists(cancel)) return 2;
  if (mode == L"--extract" && fs::exists(dest)) throw std::runtime_error("DEST_EXISTS");
  wchar_t own[MAX_PATH * 4];
  const DWORD len = GetModuleFileNameW(nullptr, own, ARRAYSIZE(own));
  if (!len || len >= ARRAYSIZE(own)) throw std::runtime_error("HELPER_ERROR");
  const fs::path extractor = fs::path(own).parent_path() / "extract-xiso.exe";
  xiso::Progress progress;
  xiso::RunResult result;
  std::atomic<bool> done{false};
  std::thread worker([&] {
    try {
      result = mode == L"--check" ? xiso::RunList(extractor, iso, progress)
                                  : xiso::RunExtract(extractor, iso, dest, progress);
    } catch (const std::exception& e) {
      result.error = e.what();
    }
    done = true;
  });
  do {
    std::error_code ec;
    if (fs::exists(cancel, ec)) progress.cancel = true;
    std::string file = progress.GetCurrentFile();
    for (char& c : file) if (c == '\r' || c == '\n' || c == '|') c = ' ';
    std::printf("PROGRESS|%u|%u|%s\n", progress.files_done.load(), total, file.c_str());
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  } while (!done);
  worker.join();
  if (!result.output.empty()) std::fprintf(stderr, "%s\n", result.output.c_str());
  if (result.canceled || progress.cancel || fs::exists(cancel)) return 2;
  if (!result.ok) throw std::runtime_error("EXTRACT_FAILED");
  if (mode == L"--check") {
    if (!result.files_total || !result.total_bytes || !xiso::OutputHasDefaultXex(result.output))
      throw std::runtime_error("INVALID_ISO");
    std::printf("BYTES|%llu\nFILES|%u\n",
                static_cast<unsigned long long>(result.total_bytes), result.files_total);
  } else {
    std::ifstream xex(dest / "default.xex", std::ios::binary);
    ValidateXex(xex, 0, fs::file_size(dest / "default.xex"));
  }
  return 0;
}
}  // namespace

int main() {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  int code = 1;
  try {
    if (argc == 3 && std::wstring(argv[1]) == L"--validate-xex") {
      std::ifstream in(fs::path(argv[2]), std::ios::binary);
      ValidateXex(in, 0, fs::file_size(argv[2]));
      code = 0;
    } else if (argc == 4 && std::wstring(argv[1]) == L"--check") {
      code = Run(argv[1], argv[2], {}, argv[3], 0);
    } else if (argc == 6 && std::wstring(argv[1]) == L"--extract") {
      code = Run(argv[1], argv[2], argv[3], argv[4], std::stoul(argv[5]));
    } else {
      throw std::runtime_error("HELPER_ERROR");
    }
  } catch (const std::exception& e) {
    std::printf("ERROR|%s\n", e.what());
  }
  LocalFree(argv);
  return code;
}
