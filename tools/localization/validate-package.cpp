// Load a translation package and rebuild both English TXU fixtures with the
// same parser/encoder used by the executable, without launching the game.
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

#include "mods/translation_data.h"

using namespace narutorise::translation;
namespace {
void Require(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
std::vector<uint8_t> Read(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  Require(bool(stream), "Cannot open fixture: " + path.string());
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), {});
}
}  // namespace
int main(int argc, char** argv) {
  try {
    Require(argc == 4,
            "Usage: narutorise_validate_translation_package <mod folder> <fixture folder> "
            "<artifact folder>");
    Package package;
    std::string error;
    Require(LoadPackage(argv[1], package, error), error);
    std::filesystem::create_directories(argv[3]);
    nlohmann::json result;
    result["package"] = package.id;
    result["dictionary_ids"] = package.texts.size();
    std::set<uint32_t> seen;
    for (const char* filename : {"_fd141210.txu", "_fd190232.txu"}) {
      const auto source = Read(std::filesystem::path(argv[2]) / filename);
      std::vector<TextRow> original;
      Require(ReadTable(source, original, error), error);
      const auto patched = PatchTable(source, package.texts);
      Require(patched.status != PatchStatus::kInvalid, patched.error);
      for (const auto& rejected : patched.rejected)
        std::cerr << "Rejected ID " << rejected.id << ": " << rejected.reason << '\n';
      Require(patched.rejected.empty(), "Package contains rejected translations");
      const auto& bytes = patched.status == PatchStatus::kRebuilt ? patched.bytes : source;
      std::vector<TextRow> rows;
      Require(ReadTable(bytes, rows, error), error);
      Require(rows.size() == original.size(), "Changed row count");
      size_t wide = 0;
      for (size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        const auto found = package.texts.find(row.id);
        Require(found != package.texts.end(), "Missing translated ID " + std::to_string(row.id));
        Require(row.text == found->second,
                "Translated text round-trip mismatch " + std::to_string(row.id));
        Require(row.id == original[index].id && row.auxiliary == original[index].auxiliary &&
                    row.source_index == original[index].source_index,
                "Changed ID, auxiliary or order");
        if (row.text == original[index].text)
          Require(row.encoded == original[index].encoded, "Changed unchanged text bytes");
        Require(seen.insert(row.id).second, "Duplicate ID across resources");
        wide += row.wide;
      }
      const auto output = std::filesystem::path(argv[3]) / filename;
      std::ofstream stream(output, std::ios::binary);
      stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      Require(bool(stream), "Cannot write rebuilt fixture");
      result["tables"][filename] = {{"entries", rows.size()},
                                    {"replacements", patched.replacements},
                                    {"original_bytes", source.size()},
                                    {"translated_bytes", bytes.size()},
                                    {"wide_texts", wide}};
    }
    Require(seen.size() == package.texts.size(), "Dictionary contains unused IDs");
    result["valid"] = true;
    std::ofstream report(std::filesystem::path(argv[3]) / "txu-validation.json");
    report << result.dump(2) << '\n';
    Require(bool(report), "Cannot write validation report");
    std::cout << result.dump(2) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Translation package FAIL: " << error.what() << '\n';
    return 1;
  }
}
