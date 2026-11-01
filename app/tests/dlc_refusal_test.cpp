#include "dlc_refusal.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs = std::filesystem;
using namespace narutorise;
namespace {
void Touch(const fs::path& path, const char* text = "x") {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}
}  // namespace
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  int checks = 0;
  const auto check = [&](bool value, const char* message) {
    ++checks; if (!value) throw std::runtime_error(message);
  };
  try {
    const auto root = fs::absolute(argv[1]) / "dlc-refusal";
    fs::remove_all(root);
    const auto dlc_root = root / "user" / "0000000000000000" / "555307E5" / "00000002";
    const auto headers = dlc_root.parent_path() / "Headers" / "00000002";
    const std::string devkit = "5BAF925F61E9E64BED129437C208D236FF5FAC4655";
    const std::string retail = "4D5307E6000000000000000000000000000000AA";
    Touch(dlc_root / devkit / "AI2C@2.DLL");                 // Another letter case.
    Touch(dlc_root / devkit / "data" / "missions.big");
    Touch(dlc_root / retail / "content" / "ai2c@3.dll");     // In a subfolder.
    Touch(headers / (devkit + ".header"));

    auto found = dlc_refusal::PackagesWithModule(dlc_root, "ai2c@2.dll");
    check(found.size() == 1 && found[0] == devkit, "The package carrying the module is found in any letter case");
    found = dlc_refusal::PackagesWithModule(dlc_root, "ai2c@3.dll");
    check(found.size() == 1 && found[0] == retail, "A module in a subfolder is found");
    check(dlc_refusal::PackagesWithModule(dlc_root, "ai2c@1.dll").empty(), "No package carries a missing module");
    check(dlc_refusal::PackagesWithModule(root / "missing", "ai2c@2.dll").empty(), "A missing root has no packages");

    const auto record = root / "data" / "dlc-refused.json";
    check(dlc_refusal::Read(record).empty(), "A missing record refuses nothing");
    fs::create_directories(record.parent_path());
    check(dlc_refusal::Add(record, {{devkit, "ai2c@2.dll"}}), "The record is written");
    check(dlc_refusal::Add(record, {{devkit, "ai2c@2.dll"}, {retail, "ai2c@3.dll"}}), "A second entry is added");
    auto refused = dlc_refusal::Read(record);
    check(refused.size() == 2 && refused[0].package == devkit && refused[0].module == "ai2c@2.dll" &&
              refused[1].package == retail, "Each package is recorded once, in order");
    check(dlc_refusal::Add(record, {{devkit, "ai2c@2.dll", "Pacote Ninja"}}), "A name joins a recorded package");
    refused = dlc_refusal::Read(record);
    check(refused.size() == 2 && refused[0].name == "Pacote Ninja" && refused[1].name.empty(),
          "Only the package without a name gains it");
    check(dlc_refusal::Add(record, {{devkit, "ai2c@2.dll", "Outro"}}) &&
              dlc_refusal::Read(record)[0].name == "Pacote Ninja", "A recorded name stays");

    // An STFS header's display name: UTF-16 big-endian at 0x411, padded.
    {
      std::string header(0x600, '\0');
      std::memcpy(header.data(), "LIVE", 4);
      const char16_t name[] = u"Ninja É 忍  ";
      for (size_t i = 0; name[i]; ++i) {
        header[0x411 + 2 * i] = char(name[i] >> 8);
        header[0x412 + 2 * i] = char(name[i] & 0xFF);
      }
      Touch(root / "stfs" / devkit, header.c_str());  // Cut at the first zero byte: write it whole below.
      std::ofstream(root / "stfs" / devkit, std::ios::binary).write(header.data(), std::streamsize(header.size()));
    }
    check(dlc_refusal::PackageDisplayName(root / "stfs" / devkit) == "Ninja \xc3\x89 \xe5\xbf\x8d",
          "The display name decodes to UTF-8 without the padding");
    Touch(root / "stfs" / "short", "LIVE");
    check(dlc_refusal::PackageDisplayName(root / "stfs" / "short").empty(), "A file without the field has no name");

    Touch(root / "data" / "broken.json", "{not json");
    check(dlc_refusal::Read(root / "data" / "broken.json").empty(), "An unreadable record refuses nothing");

    const auto refused_dir = root / "data" / "dlc-refused";
    check(dlc_refusal::MoveAside(dlc_root, headers / (devkit + ".header"), refused_dir, devkit),
          "The installed package moves aside");
    check(!fs::exists(dlc_root / devkit) && !fs::exists(headers / (devkit + ".header")),
          "The game no longer sees the package or its header");
    check(fs::is_regular_file(refused_dir / devkit / "AI2C@2.DLL") &&
              fs::is_regular_file(refused_dir / devkit / "data" / "missions.big") &&
              fs::is_regular_file(refused_dir / (devkit + ".header")), "Nothing is deleted: the copy is kept whole");
    check(!dlc_refusal::MoveAside(dlc_root, headers / (devkit + ".header"), refused_dir, devkit),
          "A package already moved is not there to move");
    Touch(dlc_root / devkit / "AI2C@2.DLL", "reinstalled");
    check(dlc_refusal::MoveAside(dlc_root, headers / (devkit + ".header"), refused_dir, devkit),
          "A copy that came back moves aside again");
    std::string text;
    {
      std::ifstream again(refused_dir / devkit / "AI2C@2.DLL");
      text.assign(std::istreambuf_iterator<char>(again), {});
    }
    check(text == "reinstalled" && !fs::exists(refused_dir / devkit / "data"), "It replaces the earlier refused copy");
    fs::remove_all(root);
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
  std::cout << "dlc refusal: " << checks << " checks passed\n";
  return 0;
}
