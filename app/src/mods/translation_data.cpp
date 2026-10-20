// TXU on disk: little-endian version/count, then count records of
// {id, auxiliary, length | wide flag}, then the terminated strings in order.
// Length counts UTF-16 units for wide strings, bytes for narrow strings;
// terminators are stored but not counted. Descriptors and offsets in the game's
// own pool are built by sub_8264E3A8, not by this codec.
//
// Keep unchanged/rejected entries byte-for-byte. Legacy narrow tables include
// CP1252 ellipses (0x85), which we decode for dictionary matching only. New text
// uses ASCII/printable Latin-1 or UTF-16LE: official French/German TXUs store
// typographic punctuation and ligatures as wide strings. Do not introduce
// CP1252 punctuation bytes whose interpretation by the game's fonts is unclear.
#include "mods/translation_data.h"

#include <utf8cpp/utf8.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string_view>
#include <toml++/toml.hpp>

namespace narutorise::translation {
namespace {
constexpr size_t kHeaderBytes = 4;
constexpr size_t kRecordBytes = 12;
constexpr size_t kIdOffset = 0;
constexpr size_t kAuxiliaryOffset = 4;
constexpr size_t kLengthOffset = 8;
constexpr uint32_t kVersion = 0x01000000;
constexpr uint32_t kCountMask = 0x00FFFFFF;
constexpr uint32_t kWideFlag = 0x80000000;
constexpr uint32_t kLengthMask = 0x7FFFFFFF;
constexpr uint32_t kMaxEntries = 65535;

constexpr std::array<char16_t, 32> kCp1252 = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

uint32_t LE32(std::span<const uint8_t> bytes, size_t at) {
  return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) | (uint32_t(bytes[at + 2]) << 16) |
         (uint32_t(bytes[at + 3]) << 24);
}
void Put32(std::vector<uint8_t>& bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    bytes.push_back(uint8_t(value >> shift));
}
std::u16string Utf16(std::string_view text) {
  std::u16string out;
  utf8::utf8to16(text.begin(), text.end(), std::back_inserter(out));
  return out;
}
std::string Utf8(const std::u16string& text) {
  std::string out;
  utf8::utf16to8(text.begin(), text.end(), std::back_inserter(out));
  return out;
}
std::string Decode(const TextRow& row) {
  std::u16string text;
  text.reserve(row.units);
  for (size_t index = 0; index < row.units; ++index) {
    if (row.wide) {
      text.push_back(
          char16_t(row.encoded[2 * index] | (uint16_t(row.encoded[2 * index + 1]) << 8)));
    } else {
      const uint8_t value = row.encoded[index];
      text.push_back(value >= 0x80 && value < 0xA0 ? kCp1252[value - 0x80] : char16_t(value));
    }
  }
  return Utf8(text);
}
void Encode(TextRow& row, std::string_view translated, const std::u16string& unicode) {
  std::vector<uint8_t> narrow;
  bool fits = !row.wide;
  if (fits) {
    for (char16_t value : unicode) {
      if (value < 0x80 || (value >= 0xA0 && value <= 0xFF)) {
        narrow.push_back(uint8_t(value));
      } else {
        // Also store C1 controls as wide, avoiding the legacy CP1252 aliases.
        fits = false;
        break;
      }
    }
  }
  row.wide = !fits;
  row.units = uint32_t(unicode.size());
  row.encoded.clear();
  if (fits) {
    row.encoded = std::move(narrow);
    row.encoded.push_back(0);
  } else {
    for (char16_t value : unicode) {
      row.encoded.push_back(uint8_t(value));
      row.encoded.push_back(uint8_t(value >> 8));
    }
    row.encoded.push_back(0);
    row.encoded.push_back(0);
  }
  row.text = translated;
}
// Keep engine commands and substitutions verbatim. Reordering commands in a
// translation is possible, but their names and multiplicities cannot change.
std::multiset<std::string> Tags(std::string_view text) {
  std::multiset<std::string> tags;
  for (size_t at = 0; (at = text.find('<', at)) != std::string_view::npos;) {
    const size_t end = text.find('>', at + 1);
    if (end == std::string_view::npos)
      break;
    tags.emplace(text.substr(at, end - at + 1));
    at = end + 1;
  }
  return tags;
}
std::multiset<std::string> Substitutions(std::string_view text) {
  std::multiset<std::string> formats;
  // The game's numbered arguments (%1, %2, ...) can change order in a
  // translation. A literal percentage ("100% of ...") is not an argument.
  for (size_t at = 0; at < text.size(); ++at) {
    if (text[at] != '%' || at + 1 == text.size() || text[at + 1] < '1' || text[at + 1] > '9')
      continue;
    size_t end = at + 2;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9')
      ++end;
    formats.emplace(text.substr(at, end - at));
    at = end - 1;
  }
  return formats;
}
bool ReadFile(const std::filesystem::path& path, std::string& text, std::string& error) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaxTableBytes) {
    error = "Cannot read bounded mod file: " + path.string();
    return false;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    error = "Cannot open mod file: " + path.string();
    return false;
  }
  text.assign(std::istreambuf_iterator<char>(stream), {});
  if (!stream.eof() && stream.fail()) {
    error = "Cannot finish reading mod file";
    return false;
  }
  if (text.size() != size) {
    error = "Mod file changed while being read";
    return false;
  }
  return true;
}
}  // namespace

bool ValidModId(std::string_view id) {
  return !id.empty() && id.size() <= 80 && std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_';
  });
}

bool ParseDictionary(std::string_view text, Dictionary& out, std::string& error) {
  try {
    if (text.size() > kMaxTableBytes)
      throw std::runtime_error("Translation JSON is too large");
    std::set<std::string> keys;
    bool duplicate = false;
    auto callback = [&](int, nlohmann::json::parse_event_t event, nlohmann::json& value) {
      if (event == nlohmann::json::parse_event_t::key &&
          !keys.insert(value.get<std::string>()).second)
        duplicate = true;
      return true;
    };
    auto json = nlohmann::json::parse(text.begin(), text.end(), callback);
    if (duplicate)
      throw std::runtime_error("Duplicate translation ID in JSON");
    if (!json.is_object() || json.size() > kMaxEntries)
      throw std::runtime_error("Expected an object with at most 65535 text IDs");
    Dictionary parsed;
    for (auto it = json.begin(); it != json.end(); ++it) {
      const auto& key = it.key();
      uint32_t id = 0;
      const auto conversion = std::from_chars(key.data(), key.data() + key.size(), id, 10);
      if (conversion.ec != std::errc{} || conversion.ptr != key.data() + key.size() ||
          key.empty() || !it.value().is_string())
        throw std::runtime_error("Expected decimal uint32 IDs and string values");
      const auto value = it.value().get<std::string>();
      if (value.find('\0') != std::string::npos || Utf16(value).size() > kMaxTextUnits)
        throw std::runtime_error("Translation contains NUL or exceeds the text limit");
      if (!parsed.emplace(id, value).second)
        throw std::runtime_error("Text IDs must be unique after decimal conversion");
    }
    out = std::move(parsed);
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

bool ReadTable(std::span<const uint8_t> input, std::vector<TextRow>& out, std::string& error) {
  try {
    if (input.size() < kHeaderBytes || input.size() > kMaxTableBytes)
      throw std::runtime_error("Invalid TXU byte count");
    const uint32_t header = LE32(input, 0);
    const uint32_t count = header & kCountMask;
    if ((header & ~kCountMask) != kVersion || count > kMaxEntries ||
        size_t(count) * kRecordBytes > input.size() - kHeaderBytes)
      throw std::runtime_error("Unsupported or truncated TXU header");
    size_t cursor = kHeaderBytes + size_t(count) * kRecordBytes;
    std::vector<TextRow> rows;
    std::set<uint32_t> ids;
    rows.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
      const size_t at = kHeaderBytes + size_t(index) * kRecordBytes;
      const uint32_t flags = LE32(input, at + kLengthOffset);
      TextRow row;
      row.source_index = index;
      row.id = LE32(input, at + kIdOffset);
      row.auxiliary = LE32(input, at + kAuxiliaryOffset);
      row.units = flags & kLengthMask;
      row.wide = (flags & kWideFlag) != 0;
      const uint64_t bytes = (uint64_t(row.units) + 1) * (row.wide ? 2 : 1);
      if (row.units > kMaxTextUnits || bytes > input.size() - cursor || !ids.insert(row.id).second)
        throw std::runtime_error("Invalid TXU text range or duplicate ID");
      row.encoded.assign(input.begin() + cursor, input.begin() + cursor + size_t(bytes));
      if (row.encoded.back() != 0 || (row.wide && row.encoded[row.encoded.size() - 2] != 0))
        throw std::runtime_error("TXU text is not terminated");
      row.text = Decode(row);
      rows.push_back(std::move(row));
      cursor += size_t(bytes);
    }
    if (cursor != input.size())
      throw std::runtime_error("Unaccounted bytes after TXU texts");
    out = std::move(rows);
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

PatchResult PatchTable(std::span<const uint8_t> input, const Dictionary& translations) {
  PatchResult result;
  if (translations.empty())
    return result;
  std::vector<TextRow> rows;
  if (!ReadTable(input, rows, result.error)) {
    result.status = PatchStatus::kInvalid;
    return result;
  }
  try {
    for (auto& row : rows) {
      const auto found = translations.find(row.id);
      if (found == translations.end() || found->second == row.text)
        continue;
      const auto& translated = found->second;
      std::u16string unicode;
      try {
        unicode = Utf16(translated);
      } catch (const utf8::exception&) {
        result.rejected.push_back({row.id, "Invalid UTF-8"});
        continue;
      }
      if (translated.find('\0') != std::string::npos || unicode.size() > kMaxTextUnits) {
        result.rejected.push_back({row.id, "Text contains NUL or exceeds the text limit"});
        continue;
      }
      if (Tags(row.text) != Tags(translated)) {
        result.rejected.push_back({row.id, "Engine tags changed"});
        continue;
      }
      if (Substitutions(row.text) != Substitutions(translated)) {
        result.rejected.push_back({row.id, "Numbered arguments changed"});
        continue;
      }
      Encode(row, translated, unicode);
      ++result.replacements;
    }
    if (result.replacements == 0)
      return result;
    Put32(result.bytes, kVersion | uint32_t(rows.size()));
    for (const auto& row : rows) {
      Put32(result.bytes, row.id);
      Put32(result.bytes, row.auxiliary);
      Put32(result.bytes, row.units | (row.wide ? kWideFlag : 0));
    }
    for (const auto& row : rows) {
      if (row.encoded.size() > kMaxTableBytes - result.bytes.size())
        throw std::runtime_error("Translated TXU exceeds the byte limit");
      result.bytes.insert(result.bytes.end(), row.encoded.begin(), row.encoded.end());
    }
    result.status = PatchStatus::kRebuilt;
  } catch (const std::exception& exception) {
    result.status = PatchStatus::kInvalid;
    result.error = exception.what();
    result.bytes.clear();
    result.replacements = 0;
  }
  return result;
}

bool LoadPackage(const std::filesystem::path& directory, Package& out, std::string& error) {
  try {
    if (!ValidModId(directory.filename().string()))
      throw std::runtime_error("Invalid translation mod folder name");
    std::string manifest;
    if (!ReadFile(directory / "mod.toml", manifest, error))
      return false;
    const auto config = toml::parse(manifest);
    if (config["mod"]["type"].value_or(std::string{}) != "translation" ||
        config["mod"]["base_language"].value_or(std::string{}) != "en")
      throw std::runtime_error("This loader requires type=translation and base_language=en");
    const auto file = config["mod"]["texts"].value_or(std::string("textos.json"));
    if (file.empty() || std::filesystem::path(file).filename() != std::filesystem::path(file) ||
        file == "." || file == "..")
      throw std::runtime_error("The texts file must be inside the mod folder");
    Package parsed;
    parsed.id = directory.filename().string();
    parsed.name = config["mod"]["name"].value_or(parsed.id);
    parsed.version = config["mod"]["version"].value_or(std::string{});
    std::string dictionary;
    if (!ReadFile(directory / file, dictionary, error) ||
        !ParseDictionary(dictionary, parsed.texts, error))
      return false;
    out = std::move(parsed);
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}
}  // namespace narutorise::translation
