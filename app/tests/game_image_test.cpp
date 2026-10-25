// Game data identification (src/game_image.h) on synthetic disc images and
// folders, then on real ones when given.
// Usage: narutorise_game_image_test <fixture dir> [<status>=<path>...]
// <status> is a StatusName: ready, other-version, other-game, not-xbox360 or
// unreadable or incomplete.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "game_image.h"

using namespace narutorise::game_image;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0;
void Check(bool value, const std::string& label) {
  ++checks;
  if (!value)
    throw std::runtime_error(label);
}

constexpr size_t kSector = 2048;

void PutBig32(std::string& data, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    data[offset + i] = char(value >> (24 - i * 8));
}
void PutLittle32(std::string& data, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    data[offset + i] = char(value >> (i * 8));
}
void PutLittle16(std::string& data, size_t offset, uint16_t value) {
  data[offset] = char(value);
  data[offset + 1] = char(value >> 8);
}

// The XEX2 header and its execution information (Title ID at +12).
std::string Xex(uint32_t title_id) {
  std::string xex(56, '\0');
  xex.replace(0, 4, "XEX2");
  PutBig32(xex, 20, 1);           // One optional header.
  PutBig32(xex, 24, 0x00040006);  // Execution information...
  PutBig32(xex, 28, 32);          // ...at offset 32.
  PutBig32(xex, 32 + 12, title_id);
  return xex;
}

// An XDVDFS image with the game partition at 0: the volume descriptor at
// sector 32, the root directory at 33 and one file per sector from 34.
std::string Image(const std::vector<std::pair<std::string, std::string>>& files) {
  std::string root(kSector, '\xFF');
  size_t entry = 0;
  uint32_t sector = 34;
  std::vector<std::pair<uint32_t, const std::string*>> data;
  for (size_t i = 0; i < files.size(); ++i) {
    const auto& [name, contents] = files[i];
    const size_t size = (14 + name.size() + 3) / 4 * 4;
    const bool last = i + 1 == files.size();
    PutLittle16(root, entry, 0);
    PutLittle16(root, entry + 2, last ? 0 : uint16_t((entry + size) / 4));
    PutLittle32(root, entry + 4, sector);
    PutLittle32(root, entry + 8, uint32_t(contents.size()));
    root[entry + 12] = char(0x20);  // Archive.
    root[entry + 13] = char(name.size());
    root.replace(entry + 14, name.size(), name);
    data.emplace_back(sector, &contents);
    sector += uint32_t((contents.size() + kSector - 1) / kSector);
    entry += size;
  }
  std::string image(size_t(sector) * kSector, '\0');
  image.replace(32 * kSector, 20, "MICROSOFT*XBOX*MEDIA");
  PutLittle32(image, 32 * kSector + 20, 33);
  PutLittle32(image, 32 * kSector + 24, uint32_t(kSector));
  image.replace(33 * kSector, kSector, root);
  for (const auto& [at, contents] : data)
    image.replace(size_t(at) * kSector, contents->size(), *contents);
  return image;
}

fs::path Utf8Path(const std::string& text) {
  return fs::path(std::u8string(text.begin(), text.end()));
}

void Write(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), std::streamsize(contents.size()));
  if (!out)
    throw std::runtime_error("Cannot write fixture " + path.string());
}

void SyntheticTests(const fs::path& dir) {
  const std::string naruto = Xex(0x555307E5);
  const std::string other = Xex(0x4D5307D8);
  Write(dir / "naruto.iso", Image({{"readme.txt", "x"}, {"default.xex", naruto}}));
  Write(dir / "other.iso", Image({{"default.xex", other}}));
  Write(dir / "no-xex.iso", Image({{"readme.txt", "x"}}));
  Write(dir / "random.bin", std::string(70 * kSector, '\x5A'));
  Write(dir / "folder" / "default.xex", naruto);

  // A synthetic XEX of the right title is still not the port's default.xex.
  const auto image = Identify(dir / "naruto.iso");
  Check(image.status == Status::kOtherVersion, "naruto.iso: other-version");
  Check(image.disc_image, "naruto.iso: disc image");
  Check(image.title_id == 0x555307E5, "naruto.iso: Title ID");
  const auto folder = Identify(dir / "folder");
  Check(folder.status == Status::kOtherVersion && !folder.disc_image, "folder: other-version");
  Check(folder.xex_digest == image.xex_digest, "folder and image: same default.xex digest");
  Check(Identify(dir / "folder" / "default.xex").status == Status::kOtherVersion,
        "default.xex itself: other-version");

  const auto other_game = Identify(dir / "other.iso");
  Check(other_game.status == Status::kOtherGame && other_game.title_id == 0x4D5307D8,
        "other.iso: other-game");
  Check(Identify(dir / "no-xex.iso").status == Status::kOtherGame, "no-xex.iso: other-game");
  Check(Identify(dir / "random.bin").status == Status::kNotXbox360, "random.bin: not-xbox360");
  Check(Identify(dir).status == Status::kNotXbox360, "folder without default.xex: not-xbox360");
  Check(Identify(dir / "missing.iso").status == Status::kUnreadable, "missing.iso: unreadable");
}

// <status>=<path>: a real disc image or folder and the status it must have.
void RealTest(const std::string& argument) {
  const auto split = argument.find('=');
  if (split == std::string::npos)
    throw std::runtime_error("Expected <status>=<path>: " + argument);
  const std::string expected = argument.substr(0, split);
  const fs::path path = Utf8Path(argument.substr(split + 1));
  const auto identity = Identify(path);
  std::cout << path.string() << ": " << StatusName(identity.status) << " title=" << std::hex
            << identity.title_id << std::dec << " xex=" << identity.xex_digest
            << (identity.disc_image ? " (disc image)" : "") << '\n';
  Check(expected == StatusName(identity.status), path.string() + ": " + expected);
}

// Use a compatible XEX supplied by the caller, keeping all fixture changes
// separate from the real installation. Placeholder data checks presence only.
void RequiredDataTests(const fs::path& dir, const fs::path& source) {
  const auto install = dir / "install";
  const auto partial = install / "game_root", complete = install / "game";
  fs::create_directories(partial);
  fs::create_directories(complete);
  fs::copy_file(source / "default.xex", partial / "default.xex");
  fs::copy_file(source / "default.xex", complete / "default.xex");
  Check(Identify(partial).status == Status::kIncomplete, "matching XEX alone is incomplete");
  Check(Identify(partial / "default.xex").status == Status::kIncomplete,
        "picking a matching XEX still requires its data");
  for (const auto* name : {"AI2C.dll", "naruto.bf", "xeshader.sdb", "soundxe.big", "soundxe.fat"})
    Write(complete / name, "fixture");
  Check(Identify(complete).status == Status::kReady, "required files accept case-insensitive disc names");
  Check(FindNearbyGame(install) == fs::canonical(complete), "skip codegen folder for complete sibling");
  const auto executable = install / "app" / "out" / "build" / "release";
  fs::create_directories(executable);
  Check(FindNearbyGame(executable) == fs::canonical(complete), "developer build finds repository game folder");
  Write(complete / "naruto.bf", "");
  Check(Identify(complete).status == Status::kIncomplete, "empty main archive is incomplete");
  Write(complete / "naruto.bf", "fixture");
  fs::remove(complete / "soundxe.fat");
  Check(Identify(complete).status == Status::kIncomplete, "missing audio index is incomplete");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2)
      throw std::runtime_error("Pass an artifacts directory for synthetic disc images");
    SyntheticTests(Utf8Path(argv[1]));
    bool data_tested = false;
    for (int i = 2; i < argc; ++i) {
      RealTest(argv[i]);
      const std::string argument(argv[i]);
      if (!data_tested && argument.starts_with("ready=")) {
        const auto root = Utf8Path(argument.substr(6));
        if (fs::is_directory(root)) {
          RequiredDataTests(Utf8Path(argv[1]), root);
          data_tested = true;
        }
      }
    }
    std::cout << "game_image PASS checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "game_image FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
