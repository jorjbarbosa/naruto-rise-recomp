#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace narutorise::translation {
using Dictionary = std::map<uint32_t, std::string>;  // UTF-8, keyed by decimal text ID.
inline constexpr size_t kMaxTableBytes = 16 * 1024 * 1024;
inline constexpr size_t kMaxTextUnits = 65535;

struct TextRow {
  uint32_t id = 0, auxiliary = 0, units = 0, source_index = 0;
  bool wide = false;
  std::vector<uint8_t> encoded;  // Original table byte order, including the terminator.
  std::string text;
};
struct Package {
  std::string id, name, version;
  Dictionary texts;
};
enum class PatchStatus { kUnchanged, kRebuilt, kInvalid };
struct RejectedText {
  uint32_t id;
  std::string reason;
};
struct PatchResult {
  PatchStatus status = PatchStatus::kUnchanged;
  std::vector<uint8_t> bytes;
  size_t replacements = 0;
  std::vector<RejectedText> rejected;
  std::string error;  // Fatal table error; rejected entries keep their original bytes.
};

bool ParseDictionary(std::string_view json, Dictionary& out, std::string& error);
bool ReadTable(std::span<const uint8_t> input, std::vector<TextRow>& out, std::string& error);
PatchResult PatchTable(std::span<const uint8_t> input, const Dictionary& translations);
bool LoadPackage(const std::filesystem::path& directory, Package& out, std::string& error);
bool ValidModId(std::string_view id);
}  // namespace narutorise::translation
