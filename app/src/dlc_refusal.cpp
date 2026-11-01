// The DLC packages refused for a devkit module (dlc_refusal.h).
#include "dlc_refusal.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>

namespace narutorise::dlc_refusal {
namespace fs = std::filesystem;
namespace {
std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : char(c); });
  return text;
}

// A move that survives another volume: the data folder and the content folder
// may be on different drives.
bool Move(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::rename(from, to, ec);
  if (!ec) return true;
  ec.clear();
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) return false;
  fs::remove_all(from, ec);
  return !ec;
}
}  // namespace

std::vector<std::string> PackagesWithModule(const fs::path& dlc_root, std::string_view module) {
  std::vector<std::string> packages;
  const std::string wanted = Lower(std::string(module));
  std::error_code ec;
  for (const auto& package : fs::directory_iterator(dlc_root, ec)) {
    if (!package.is_directory(ec)) continue;
    for (fs::recursive_directory_iterator it(package.path(), ec), end; !ec && it != end; it.increment(ec)) {
      if (it->is_regular_file(ec) && Lower(it->path().filename().string()) == wanted) {
        packages.push_back(package.path().filename().string());
        break;
      }
    }
    ec.clear();
  }
  return packages;
}

std::vector<Refused> Read(const fs::path& record) {
  std::vector<Refused> refused;
  std::ifstream file(record, std::ios::binary);
  if (!file) return refused;
  const auto json = nlohmann::json::parse(file, nullptr, false);
  if (!json.is_object() || !json.contains("refused") || !json["refused"].is_array()) return refused;
  for (const auto& entry : json["refused"]) {
    if (!entry.is_object()) continue;
    Refused item{entry.value("package", std::string()), entry.value("module", std::string()),
                 entry.value("name", std::string())};
    if (!item.package.empty()) refused.push_back(std::move(item));
  }
  return refused;
}

bool Add(const fs::path& record, const std::vector<Refused>& entries) {
  auto refused = Read(record);
  for (const auto& entry : entries) {
    const auto known = std::find_if(refused.begin(), refused.end(),
                                    [&](const Refused& r) { return r.package == entry.package; });
    if (known == refused.end()) refused.push_back(entry);
    else if (known->name.empty()) known->name = entry.name;
  }
  auto list = nlohmann::json::array();
  for (const auto& entry : refused)
    list.push_back({{"package", entry.package}, {"module", entry.module}, {"name", entry.name}});
  const fs::path temporary = fs::path(record).concat(".tmp");
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << nlohmann::json{{"refused", list}}.dump(2) << '\n';
    if (!file) return false;
  }
  std::error_code ec;
  fs::rename(temporary, record, ec);
  return !ec;
}

std::string PackageDisplayName(const fs::path& package) {
  constexpr std::streamoff kDisplayName = 0x411;
  constexpr size_t kChars = 0x80 / 2;  // One locale's field.
  std::ifstream file(package, std::ios::binary);
  if (!file.seekg(kDisplayName)) return {};
  unsigned char raw[kChars * 2] = {};
  if (!file.read(reinterpret_cast<char*>(raw), sizeof(raw))) return {};
  std::string name;
  for (size_t i = 0; i < kChars; ++i) {
    uint32_t code = uint32_t(raw[2 * i]) << 8 | raw[2 * i + 1];
    if (code == 0) break;
    if (code >= 0xD800 && code < 0xDC00 && i + 1 < kChars) {  // A surrogate pair.
      const uint32_t low = uint32_t(raw[2 * i + 2]) << 8 | raw[2 * i + 3];
      if (low < 0xDC00 || low >= 0xE000) return {};
      code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
      ++i;
    } else if (code >= 0xD800 && code < 0xE000) {
      return {};
    }
    if (code < 0x80) {
      name += char(code);
    } else if (code < 0x800) {
      name += char(0xC0 | code >> 6);
      name += char(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
      name += char(0xE0 | code >> 12);
      name += char(0x80 | (code >> 6 & 0x3F));
      name += char(0x80 | (code & 0x3F));
    } else {
      name += char(0xF0 | code >> 18);
      name += char(0x80 | (code >> 12 & 0x3F));
      name += char(0x80 | (code >> 6 & 0x3F));
      name += char(0x80 | (code & 0x3F));
    }
  }
  // Trailing spaces pad some packages' names.
  while (!name.empty() && name.back() == ' ') name.pop_back();
  return name;
}

bool MoveAside(const fs::path& dlc_root, const fs::path& header, const fs::path& refused_dir,
               std::string_view package) {
  std::error_code ec;
  const fs::path folder = dlc_root / package;
  if (!fs::is_directory(folder, ec)) return false;
  fs::create_directories(refused_dir, ec);
  const fs::path target = refused_dir / package;
  fs::remove_all(target, ec);  // An earlier refused copy of the same package.
  if (!Move(folder, target)) return false;
  if (fs::is_regular_file(header, ec)) Move(header, fs::path(target).concat(".header"));
  return true;
}
}  // namespace narutorise::dlc_refusal
