#include "installer_payload.h"

#include <cstring>
#include <fstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace installer_payload {
namespace {

std::filesystem::path g_installer_dir;
std::filesystem::path g_embedded_dir;
bool g_has_embedded = false;
bool g_embedded_checked = false;

uint32_t ReadU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) {
    v = (v << 8) | p[i];
  }
  return v;
}

bool SafeRelativePath(const std::string& rel) {
  if (rel.empty()) return false;
  if (rel.find("..") != std::string::npos) return false;
  // Only allow a plain relative path (no drive letters, no UNC).
  size_t start = 0;
  if (rel[0] == '/' || rel[0] == '\\') start = 1;
  std::string body = rel.substr(start);
  if (body.size() >= 2 && body[1] == ':') return false;
  return true;
}

}  // namespace

std::filesystem::path OwnExePath() {
#if defined(_WIN32)
  wchar_t buf[MAX_PATH * 4];
  DWORD len = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
  if (len == 0 || len >= ARRAYSIZE(buf)) return {};
  return std::filesystem::path(std::wstring(buf, buf + len));
#else
  return {};
#endif
}

std::filesystem::path InstallerDir() {
  if (g_installer_dir.empty()) {
    std::filesystem::path exe = OwnExePath();
    if (!exe.empty()) {
      g_installer_dir = exe.parent_path();
    } else {
      g_installer_dir = std::filesystem::current_path();
    }
  }
  return g_installer_dir;
}

bool ExtractEmbedded(std::string& error) {
  if (g_embedded_checked) return g_has_embedded;
  g_embedded_checked = true;

  std::filesystem::path exe = OwnExePath();
  if (exe.empty()) return false;

  std::ifstream in(exe, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const uint64_t file_size = static_cast<uint64_t>(in.tellg());

  constexpr size_t kTrailerSize = sizeof(uint64_t) + kMarkerSize;
  if (file_size < kTrailerSize + 4) return false;

  std::vector<uint8_t> trailer(kTrailerSize);
  in.seekg(static_cast<std::streamoff>(file_size - kTrailerSize), std::ios::beg);
  in.read(reinterpret_cast<char*>(trailer.data()),
          static_cast<std::streamsize>(trailer.size()));
  if (static_cast<size_t>(in.gcount()) != trailer.size()) return false;

  if (std::memcmp(trailer.data() + sizeof(uint64_t), kMarker, kMarkerSize) != 0) {
    return false;  // no embedded payload: plain installer next to its files
  }

  const uint64_t payload_size = ReadU64(trailer.data());
  if (payload_size < 4 || payload_size > file_size - kTrailerSize) {
    error = "corrupt installer payload trailer";
    return false;
  }
  const uint64_t payload_offset = file_size - kTrailerSize - payload_size;

  // Unpack the payload archive to a temp folder:
  //   [int32 count] ([int32 path_len][path utf8][int64 file_len][bytes])*
  std::error_code ec;
  std::filesystem::path temp_root =
      std::filesystem::temp_directory_path(ec) / "narutorise_setup_payload";
  if (ec) {
    error = "failed to resolve the temp directory";
    return false;
  }
  // A stale unpack from a previous run is replaced wholesale.
  std::filesystem::remove_all(temp_root, ec);
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    error = "failed to create the payload temp directory";
    return false;
  }
  if (g_embedded_dir.empty()) {
    g_embedded_dir = temp_root;
  }

  in.seekg(static_cast<std::streamoff>(payload_offset), std::ios::beg);
  std::vector<uint8_t> head(4);
  in.read(reinterpret_cast<char*>(head.data()), 4);
  if (static_cast<size_t>(in.gcount()) != 4) {
    error = "corrupt installer payload header";
    return false;
  }
  const uint32_t count = ReadU32(head.data());

  std::vector<uint8_t> name_buf;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t len_buf[12];
    in.read(reinterpret_cast<char*>(len_buf), 12);
    if (static_cast<size_t>(in.gcount()) != 12) {
      error = "corrupt installer payload record";
      return false;
    }
    const int32_t path_len = static_cast<int32_t>(ReadU32(len_buf));
    const uint64_t file_len = ReadU64(len_buf + 4);
    if (path_len <= 0 || path_len > 4096 || file_len > (1ull << 40)) {
      error = "corrupt installer payload record";
      return false;
    }
    name_buf.assign(static_cast<size_t>(path_len), 0);
    in.read(reinterpret_cast<char*>(name_buf.data()), path_len);
    if (static_cast<size_t>(in.gcount()) != static_cast<size_t>(path_len)) {
      error = "corrupt installer payload record";
      return false;
    }
    std::string rel(name_buf.begin(), name_buf.end());
    if (!SafeRelativePath(rel)) {
      error = "unsafe path inside the installer payload";
      return false;
    }

    std::filesystem::path dst = g_embedded_dir / std::filesystem::path(rel);
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) {
      error = "failed to create " + dst.string();
      return false;
    }
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "failed to write " + dst.string();
      return false;
    }
    constexpr size_t kChunk = 4 * 1024 * 1024;
    std::vector<char> copy_buf(kChunk);
    uint64_t remaining = file_len;
    while (remaining > 0) {
      size_t take = static_cast<size_t>(remaining < kChunk ? remaining : kChunk);
      in.read(copy_buf.data(), static_cast<std::streamsize>(take));
      if (static_cast<size_t>(in.gcount()) != take) {
        error = "corrupt installer payload data";
        return false;
      }
      out.write(copy_buf.data(), static_cast<std::streamsize>(take));
      remaining -= take;
    }
  }

  g_has_embedded = true;
  return true;
}

bool HasEmbedded() {
  std::string err;
  return ExtractEmbedded(err);
}

std::filesystem::path Resolve(const std::filesystem::path& relative) {
  HasEmbedded();
  if (g_has_embedded) {
    std::error_code ec;
    std::filesystem::path embedded = g_embedded_dir / relative;
    if (std::filesystem::exists(embedded, ec)) return embedded;
  }
  return InstallerDir() / relative;
}

bool Available() {
  HasEmbedded();
  if (g_has_embedded) return true;
  std::error_code ec;
  return std::filesystem::exists(InstallerDir() / "narutorise.exe", ec);
}

}  // namespace installer_payload
