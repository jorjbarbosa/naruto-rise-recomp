// The check of the game files (src/game_files.h) on synthetic folders and disc
// images, then on real ones when given; also writes the manifest.
// Usage: narutorise_game_files_test <fixture dir> [<verdict>=<path>...]
//        narutorise_game_files_test manifest <root>
// <verdict> is a VerdictName (ok, damaged, other_copy, unchecked); a real
// check uses the embedded manifest. manifest prints the entries of
// game_files_manifest.inc for the files of a folder or disc image.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/hash.h>
#include <rex/system/xtypes.h>

#include "game_files.h"

using namespace narutorise::game_files;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0;
void Check(bool value, const std::string& label) {
  ++checks;
  if (!value)
    throw std::runtime_error(label);
}

constexpr size_t kSector = 2048;

void PutLittle32(std::string& data, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    data[offset + i] = char(value >> (i * 8));
}
void PutLittle16(std::string& data, size_t offset, uint16_t value) {
  data[offset] = char(value);
  data[offset + 1] = char(value >> 8);
}

// An XDVDFS image with the game partition at 0: the volume descriptor at
// sector 32, the root directory at 33 and the files from 34 (as in
// game_image_test.cpp).
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

// The manifest of the fixture files, with storage for its views.
struct Fixture {
  std::vector<std::pair<std::string, std::string>> files;  // Path, contents.
  std::vector<std::string> digests;
  std::vector<File> manifest;
  explicit Fixture(std::vector<std::pair<std::string, std::string>> list) : files(std::move(list)) {
    for (const auto& [path, contents] : files) digests.push_back(rex::hash_bytes(contents));
    for (size_t i = 0; i < files.size(); ++i)
      manifest.push_back({files[i].first, files[i].second.size(), digests[i]});
  }
  void WriteTo(const fs::path& root) const {
    fs::remove_all(root);
    for (const auto& [path, contents] : files) Write(root / Utf8Path(path), contents);
  }
};

std::string Contents(char fill, size_t size) {
  std::string data(size, fill);
  for (size_t i = 0; i < size; i += 97) data[i] = char(i * 31);
  return data;
}

// Replaces one byte, keeping the size and the write time.
void Corrupt(const fs::path& path) {
  const auto time = fs::last_write_time(path);
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(5);
  file.put('\x7F');
  file.close();
  fs::last_write_time(path, time);
}

void FolderTests(const fs::path& dir) {
  // A file larger than the check's read chunk (4 MiB) too.
  const Fixture fixture({{"default.xex", Contents('x', 4096)},
                         {"Movie.bik", Contents('m', (size_t(4) << 20) + 12345)},
                         {"naruto.bf", Contents('b', 20000)},
                         {"media/Font.ttf", Contents('f', 3000)}});
  const std::atomic<bool> running{false}, stopped{true};
  const auto root = dir / "folder";
  fixture.WriteTo(root);

  auto report = Verify(root, fixture.manifest, running);
  Check(report.verdict == Verdict::kOk && report.checked == 4 && report.Different().empty(), "folder: ok");
  Check(Tag(report) == "ok", "folder: ok tag");
  Check(Verify(root / "default.xex", fixture.manifest, running).verdict == Verdict::kOk,
        "default.xex itself: its folder");
  Check(Verify(root, fixture.manifest, stopped).verdict == Verdict::kUnchecked, "stopped: unchecked");
  Check(Verify(dir / "nowhere", fixture.manifest, running).verdict == Verdict::kUnchecked, "missing root: unchecked");

  // Case-insensitive names, as the runtime resolves them.
  fs::rename(root / "Movie.bik", root / "MOVIE.BIK");
  Check(Verify(root, fixture.manifest, running).verdict == Verdict::kOk, "folder: names in another case");

  Corrupt(root / "MOVIE.BIK");
  report = Verify(root, fixture.manifest, running);
  Check(report.verdict == Verdict::kDamaged && report.damaged == std::vector<std::string>{"Movie.bik"} &&
        report.resized.empty() && report.missing.empty(), "folder: a changed byte is damaged");
  Check(Tag(report) == "damaged:1", "folder: damaged tag");

  Write(root / "naruto.bf", Contents('b', 20001));
  fs::remove(root / "media" / "Font.ttf");
  report = Verify(root, fixture.manifest, running);
  Check(report.verdict == Verdict::kDamaged && report.resized == std::vector<std::string>{"naruto.bf"} &&
        report.missing == std::vector<std::string>{"media/Font.ttf"}, "folder: resized and missing");
  Check(report.Different() == std::vector<std::string>{"Movie.bik", "naruto.bf", "media/Font.ttf"},
        "folder: the different files in order");
  Check(Tag(report) == "damaged:3", "folder: three different");

  // Half of the files of another size: another release.
  fixture.WriteTo(root);
  Write(root / "naruto.bf", "translated");
  Write(root / "media" / "Font.ttf", "other font");
  Check(Verify(root, fixture.manifest, running).verdict == Verdict::kOtherCopy, "folder: half resized is other_copy");
}

void ImageTests(const fs::path& dir) {
  const Fixture fixture({{"default.xex", Contents('x', 6000)}, {"Movie.bik", Contents('m', 50000)},
                         {"naruto.bf", Contents('b', 3000)}});
  const std::atomic<bool> running{false};
  Write(dir / "game.iso", Image(fixture.files));
  Check(Verify(dir / "game.iso", fixture.manifest, running).verdict == Verdict::kOk, "image: ok");

  auto changed = fixture.files;
  changed[1].second[100] ^= 1;
  Write(dir / "damaged.iso", Image(changed));
  const auto report = Verify(dir / "damaged.iso", fixture.manifest, running);
  Check(report.verdict == Verdict::kDamaged && report.damaged == std::vector<std::string>{"Movie.bik"},
        "image: a changed byte is damaged");

  Write(dir / "partial.iso", Image({fixture.files[0], fixture.files[2]}));
  Check(Verify(dir / "partial.iso", fixture.manifest, running).missing == std::vector<std::string>{"Movie.bik"},
        "image: missing file");
}

std::optional<Report> Wait(Checker& checker) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (checker.Running() && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return checker.Result();
}

void CheckerTests(const fs::path& dir) {
  const Fixture fixture({{"default.xex", Contents('x', 4096)}, {"Movie.bik", Contents('m', 70000)}});
  const auto root = dir / "cached", cache = dir / "data" / "game-files.json";
  fixture.WriteTo(root);
  fs::remove(cache);
  {
    Checker checker(fixture.manifest);
    checker.Start(root, cache);
    const auto report = Wait(checker);
    Check(report && report->verdict == Verdict::kOk && !report->notified && fs::exists(cache),
          "checker: ok, saved");
    Check(checker.Generation() == 1, "checker: one report");
  }
  // A change that keeps the size and the write time is not seen: the cache
  // holds the copy's report until the copy changes, or a forced check.
  Corrupt(root / "Movie.bik");
  {
    Checker checker(fixture.manifest);
    checker.Start(root, cache);
    auto report = Wait(checker);
    Check(report && report->verdict == Verdict::kOk, "checker: the same copy comes from the cache");
    checker.Start(root, cache, true);
    report = Wait(checker);
    Check(report && report->verdict == Verdict::kDamaged && report->notified, "checker: a forced check reads again");
  }
  fixture.WriteTo(root);
  Write(root / "Movie.bik", Contents('n', 70000));
  {
    Checker checker(fixture.manifest);
    checker.Start(root, cache);
    auto report = Wait(checker);
    Check(report && report->verdict == Verdict::kDamaged && !report->notified, "checker: a new copy is read");
    checker.MarkNotified();
    Check(checker.Result()->notified, "checker: notified");
  }
  {
    Checker checker(fixture.manifest);
    checker.Start(root, cache);
    const auto report = Wait(checker);
    Check(report && report->verdict == Verdict::kDamaged && report->notified, "checker: the notice is kept");
  }
  const Fixture other({{"default.xex", Contents('x', 4096)}});
  Check(Fingerprint(root, fixture.manifest) != Fingerprint(root, other.manifest),
        "fingerprint: another manifest is another check");
  Check(Fingerprint(dir / "nowhere", fixture.manifest).empty(), "fingerprint: a missing root has none");
}

// <verdict>=<path>: a real disc image or folder against the embedded manifest.
void RealTest(const std::string& argument) {
  const auto split = argument.find('=');
  if (split == std::string::npos)
    throw std::runtime_error("Expected <verdict>=<path>: " + argument);
  const std::string expected = argument.substr(0, split);
  const fs::path path = Utf8Path(argument.substr(split + 1));
  const std::atomic<bool> running{false};
  const auto report = Verify(path, running);
  std::cout << path.string() << ": " << Tag(report) << " (" << report.checked << " files, " << report.seconds << " s)";
  for (const auto& file : report.Different()) std::cout << ' ' << file;
  std::cout << '\n';
  Check(expected == VerdictName(report.verdict), path.string() + ": " + expected);
}

// The files of a folder or disc image, under every directory but $SystemUpdate.
void Collect(const rex::filesystem::Entry& entry, const std::string& prefix,
             std::vector<std::pair<std::string, const rex::filesystem::Entry*>>& files) {
  for (const auto& child : entry.children()) {
    const std::string path = prefix + child->name();
    if (child->attributes() & rex::filesystem::kFileAttributeDirectory) {
      if (prefix.empty() && child->name() == "$SystemUpdate") continue;
      Collect(*child, path + "/", files);
    } else {
      files.emplace_back(path, child.get());
    }
  }
}

int Manifest(const fs::path& root) {
  std::unique_ptr<rex::filesystem::Device> device;
  if (fs::is_directory(root))
    device = std::make_unique<rex::filesystem::HostPathDevice>("\\Device\\GameFiles", root, true);
  else
    device = std::make_unique<rex::filesystem::DiscImageDevice>("\\Device\\GameFiles", root);
  if (!device->Initialize() || !device->ResolvePath(""))
    throw std::runtime_error("Cannot open " + root.string());
  std::vector<std::pair<std::string, const rex::filesystem::Entry*>> files;
  Collect(*device->ResolvePath(""), "", files);
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  // Each file whole through rex::hash_bytes: the check's streamed digest must agree.
  for (const auto& [path, entry] : files) {
    rex::filesystem::File* file = nullptr;
    std::string data(entry->size(), char(0));
    size_t read = 0;
    if (XFAILED(const_cast<rex::filesystem::Entry*>(entry)->Open(rex::filesystem::FileAccess::kFileReadData, &file)) ||
        !file)
      throw std::runtime_error("Cannot open " + path);
    const bool ok = data.empty() ||
        (XSUCCEEDED(file->ReadSync(std::span(reinterpret_cast<uint8_t*>(data.data()), data.size()), 0, &read)) &&
         read == data.size());
    file->Destroy();
    if (!ok)
      throw std::runtime_error("Cannot read " + path);
    std::cout << "    {\"" << path << "\", " << entry->size() << ", \"" << rex::hash_bytes(data) << "\"},\n";
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "manifest")
      return Manifest(Utf8Path(argv[2]));
    if (argc < 2)
      throw std::runtime_error("Pass an artifacts directory for the synthetic games");
    const auto dir = Utf8Path(argv[1]);
    FolderTests(dir);
    ImageTests(dir);
    CheckerTests(dir);
    for (int i = 2; i < argc; ++i) RealTest(argv[i]);
    std::cout << "game_files PASS checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "game_files FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
