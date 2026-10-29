// narutorise - ReXGlue Recompiled Project
//
// Game data identification (game_image.h): the Title ID and digest of the
// default.xex in a disc image or a folder.

#include "game_image.h"

#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/hash.h>
#include <rex/system/xtypes.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>

namespace narutorise::game_image {
namespace {

constexpr uint32_t kTitleId = 0x555307E5;  // Naruto: Rise of a Ninja
// XXH3-128 of the default.xex the code was recompiled from (game_root, the
// entrypoint of narutorise_manifest.toml). Regenerating from another XEX
// changes it; the game_image test checks it against game_root.
constexpr std::string_view kXexDigest = "8ff9ceaf48411fcc02a1f75205a9fbf6";
// Core disc data needed during boot. A matching XEX alone is also found in
// the developer's code-generation folder and does not make it playable.
constexpr std::array<std::string_view, 5> kRequiredFiles = {
    "ai2c.dll", "naruto.bf", "xeshader.sdb", "soundxe.big", "soundxe.fat"};

bool HasGameFiles(const std::filesystem::path& folder) {
  std::array<bool, kRequiredFiles.size()> found{};
  std::error_code ec;
  std::filesystem::directory_iterator it(folder.empty() ? "." : folder, ec), end;
  while (!ec && it != end) {
    const auto& entry = *it;
    const auto utf8 = entry.path().filename().u8string();
    std::string name(utf8.begin(), utf8.end());
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    const auto required = std::find(kRequiredFiles.begin(), kRequiredFiles.end(), name);
    if (required != kRequiredFiles.end() && entry.is_regular_file(ec) && entry.file_size(ec) > 0 && !ec)
      found[size_t(required - kRequiredFiles.begin())] = true;
    it.increment(ec);
  }
  return !ec && std::all_of(found.begin(), found.end(), [](bool value) { return value; });
}

uint32_t Big32(std::string_view data, size_t offset) {
  const auto* p = reinterpret_cast<const uint8_t*>(data.data()) + offset;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// The Title ID of the XEX2 execution information optional header (0x00040006).
std::optional<uint32_t> XexTitleId(std::string_view xex) {
  if (xex.size() < 24 || xex.substr(0, 4) != "XEX2") return std::nullopt;
  const uint32_t count = Big32(xex, 20);
  if (count > 4096 || 24ull + count * 8ull > xex.size()) return std::nullopt;
  for (uint32_t i = 0; i < count; ++i) {
    if (Big32(xex, 24 + i * 8) != 0x00040006) continue;
    const uint32_t execution = Big32(xex, 24 + i * 8 + 4);
    if (execution > xex.size() || xex.size() - execution < 16) return std::nullopt;
    return Big32(xex, execution + 12);
  }
  return std::nullopt;
}

bool IsDefaultXex(const std::filesystem::path& path) {
  std::string name = rex::path_to_utf8(path.filename());
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return name == "default.xex";
}

std::optional<std::string> ReadHostFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (in.bad()) return std::nullopt;
  return data;
}

// default.xex of a disc image, through the same device the runtime mounts.
std::optional<std::string> ReadImageXex(rex::filesystem::DiscImageDevice& device) {
  auto* entry = device.ResolvePath("default.xex");
  if (!entry) return std::nullopt;
  rex::filesystem::File* file = nullptr;
  if (XFAILED(entry->Open(rex::filesystem::FileAccess::kFileReadData, &file)) || !file)
    return std::nullopt;
  std::string data(entry->size(), '\0');
  size_t read = 0;
  const bool ok = data.empty() ||
      (XSUCCEEDED(file->ReadSync(std::span(reinterpret_cast<uint8_t*>(data.data()), data.size()), 0,
                                 &read)) && read == data.size());
  file->Destroy();
  if (!ok) return std::nullopt;
  return data;
}

Identity FromXex(std::string_view xex, bool disc_image) {
  Identity identity;
  identity.disc_image = disc_image;
  identity.title_id = XexTitleId(xex).value_or(0);
  identity.xex_digest = rex::hash_bytes(xex);
  if (identity.title_id != kTitleId)
    identity.status = Status::kOtherGame;
  else if (identity.xex_digest != kXexDigest)
    identity.status = Status::kOtherVersion;
  else
    identity.status = Status::kReady;
  return identity;
}

Identity FromFolderXex(std::string_view xex, const std::filesystem::path& folder) {
  auto identity = FromXex(xex, false);
  if (identity.status == Status::kReady && !HasGameFiles(folder)) identity.status = Status::kIncomplete;
  return identity;
}

}  // namespace

Identity Identify(const std::filesystem::path& root) {
  std::error_code ec;
  if (std::filesystem::is_directory(root, ec)) {
    const auto xex_path = root / "default.xex";
    if (!std::filesystem::is_regular_file(xex_path, ec)) return {.status = Status::kNotXbox360};
    const auto xex = ReadHostFile(xex_path);
    return xex ? FromFolderXex(*xex, root) : Identity{};
  }
  if (!std::filesystem::is_regular_file(root, ec)) return {};
  if (IsDefaultXex(root)) {
    const auto xex = ReadHostFile(root);
    return xex ? FromFolderXex(*xex, root.parent_path()) : Identity{};
  }
  if (!std::ifstream(root, std::ios::binary)) return {};

  rex::filesystem::DiscImageDevice device("\\Device\\GameImage", root);
  if (!device.Initialize()) return {.status = Status::kNotXbox360, .disc_image = false};
  const auto xex = ReadImageXex(device);
  if (!xex) return {.status = Status::kOtherGame, .disc_image = true};
  auto identity = FromXex(*xex, true);
  if (identity.status == Status::kReady) {
    for (const auto name : kRequiredFiles) {
      const auto* entry = device.ResolvePath(std::string(name));
      if (!entry || !entry->size() || (entry->attributes() & rex::filesystem::kFileAttributeDirectory)) {
        identity.status = Status::kIncomplete;
        break;
      }
    }
  }
  return identity;
}

std::filesystem::path FindNearbyGame(const std::filesystem::path& executable_folder) {
  const std::filesystem::path candidates[] = {
      executable_folder / "game_root", executable_folder / "game",
      executable_folder / "../../../../game_root", executable_folder / "../../../../game",
      executable_folder / "../../../game_root", executable_folder / "../../../game"};
  std::filesystem::path first;
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (!std::filesystem::is_directory(candidate, ec)) continue;
    const auto root = std::filesystem::canonical(candidate, ec);
    if (ec) continue;
    if (first.empty()) first = root;
    if (Identify(root).status == Status::kReady) return root;
  }
  // Keep an incomplete/unsupported installation visible in the setup screen.
  return first;
}

const char* StatusName(Status status) {
  switch (status) {
    case Status::kReady:
      return "ready";
    case Status::kOtherVersion:
      return "other-version";
    case Status::kOtherGame:
      return "other-game";
    case Status::kNotXbox360:
      return "not-xbox360";
    case Status::kUnreadable:
      return "unreadable";
    case Status::kIncomplete:
      return "incomplete";
  }
  return "unreadable";
}

}  // namespace narutorise::game_image
