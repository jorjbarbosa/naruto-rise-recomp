#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "mods/translation_data.h"

using namespace narutorise::translation;
namespace {
unsigned checks = 0;
void Check(bool value, const char* label) {
  ++checks;
  if (!value)
    throw std::runtime_error(label);
}
void Put32(std::vector<uint8_t>& data, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    data.push_back(uint8_t(value >> shift));
}
std::vector<uint8_t> Fixture() {
  std::vector<uint8_t> data;
  Put32(data, 0x01000003);
  Put32(data, 15132);
  Put32(data, 0x12345678);
  Put32(data, 7);
  Put32(data, 19369);
  Put32(data, 0x87654321);
  Put32(data, 0x80000003);
  Put32(data, 99);
  Put32(data, 0x00BF0237);
  Put32(data, 9);
  for (char value : std::string("OPTIONS"))
    data.push_back(uint8_t(value));
  data.push_back(0);
  for (char value : std::string("Old")) {
    data.push_back(uint8_t(value));
    data.push_back(0);
  }
  data.push_back(0);
  data.push_back(0);
  for (char value : std::string("Untouched"))
    data.push_back(uint8_t(value));
  data.push_back(0);
  return data;
}
std::vector<TextRow> Read(const std::vector<uint8_t>& bytes) {
  std::vector<TextRow> rows;
  std::string error;
  Check(ReadTable(bytes, rows, error), error.c_str());
  return rows;
}
std::vector<uint8_t> NarrowTable(std::string_view text) {
  std::vector<uint8_t> data;
  Put32(data, 0x01000001);
  Put32(data, 1);
  Put32(data, 0);
  Put32(data, uint32_t(text.size()));
  data.insert(data.end(), text.begin(), text.end());
  data.push_back(0);
  return data;
}
void CheckRejected(const PatchResult& result, uint32_t id) {
  Check(result.status == PatchStatus::kUnchanged && result.bytes.empty() &&
            result.replacements == 0 && result.error.empty(),
        "invalid entry must leave the original table usable");
  Check(result.rejected.size() == 1 && result.rejected[0].id == id &&
            !result.rejected[0].reason.empty(),
        "rejection must identify the text ID and reason");
}
void JsonTests() {
  Dictionary out{{42, "original"}};
  std::string error;
  for (std::string_view bad :
       {R"([])", R"({"-1":"x"})", R"({"4294967296":"x"})", R"({"1":false})", R"({"1":"a","1":"b"})",
        R"({"01":"a","1":"b"})", R"({"1":"\u0000"})", R"({"1":"\ud800"})", R"({"1":"x",})"}) {
    Check(!ParseDictionary(bad, out, error), "malformed JSON accepted");
    Check(out == Dictionary{{42, "original"}}, "failed JSON mutated dictionary");
  }
  Check(ParseDictionary(R"({"15132":"OP\u00c7\u00d5ES","19369":"\u65e5\u672c"})", out, error),
        "valid JSON rejected");
  Check(out.at(15132) ==
            "OP\xC3\x87\xC3\x95"
            "ES",
        "UTF-8 JSON changed");
  Check(ParseDictionary("\xEF\xBB\xBF{\"15132\":\"BOM\"}", out, error) && out.at(15132) == "BOM",
        "UTF-8 BOM rejected");
  Check(!ValidModId("../outside") && !ValidModId("C:\\outside") && ValidModId("pt-br-demo"),
        "mod ID validation");
}
void TableTests() {
  const auto original = Fixture();
  const auto before = original;
  const auto old = Read(original);
  Dictionary text{{15132,
                   "OP\xC3\x87\xC3\x95"
                   "ES DE JOGO"},
                  {19369, "\xE6\x97\xA5\xE6\x9C\xAC"},
                  {777, "unused"}};
  const auto patched = PatchTable(original, text);
  Check(patched.status == PatchStatus::kRebuilt && patched.replacements == 2, "table not rebuilt");
  const auto rows = Read(patched.bytes);
  Check(original == before, "input modified");
  Check(rows.size() == 3 && rows[0].id == old[0].id && rows[0].auxiliary == old[0].auxiliary,
        "ID/auxiliary changed");
  Check(rows[0].text == text.at(15132) && !rows[0].wide && rows[0].units == 14,
        "Latin-1 translation/longer size");
  Check(rows[0].encoded[2] == 0xC7 && rows[0].encoded[3] == 0xD5, "Portuguese bytes");
  Check(rows[1].text == text.at(19369) && rows[1].wide && rows[1].units == 2, "wide text changed");
  Check(rows[1].auxiliary == old[1].auxiliary && rows[2].encoded == old[2].encoded,
        "untranslated data changed");
  Check(PatchTable(original, {{999, "absent"}}).status == PatchStatus::kUnchanged,
        "missing ID did not fall back");
  Check(PatchTable(original, {{15132, "OPTIONS"}}).status == PatchStatus::kUnchanged,
        "same text rebuilt");
  auto unicode = PatchTable(original, {{15132, "\xF0\x9F\x8D\xA5"}});
  const auto wide = Read(unicode.bytes);
  Check(wide[0].wide && wide[0].units == 2 && wide[0].encoded.size() == 6,
        "supplementary Unicode units");
  const auto empty = PatchTable(original, {{15132, ""}});
  Check(Read(empty.bytes)[0].units == 0, "empty translation rejected");
  std::string error;
  std::vector<TextRow> ignored;
  for (size_t size = 0; size < original.size(); ++size)
    Check(!ReadTable(std::span(original).first(size), ignored, error), "truncated table accepted");
  auto invalid = original;
  invalid[12] = 0xFF;
  invalid[13] = 0xFF;
  invalid[14] = 0xFF;
  invalid[15] = 0x7F;
  Check(PatchTable(invalid, text).status == PatchStatus::kInvalid, "overflow length accepted");
  invalid = original;
  invalid.push_back(0);
  Check(!ReadTable(invalid, ignored, error), "trailing data accepted");
  CheckRejected(PatchTable(original, {{15132, "<x>"}}), 15132);
  CheckRejected(PatchTable(original, {{15132, "%1"}}), 15132);
}
void SubstitutionTests() {
  const auto source = NarrowTable("Unlock %1 in exchange for %2 Gold Coins?");
  for (const char* bad : {"Unlock item for %2", "%1 %1 %2", "%1 %2 %3", "%1 %20"})
    CheckRejected(PatchTable(source, {{1, bad}}), 1);
  Check(PatchTable(source, {{1, "Por %2 moedas, desbloquear %1?"}}).status == PatchStatus::kRebuilt,
        "reordered numbered arguments rejected");
  CheckRejected(PatchTable(NarrowTable("RAMEN DELIVERY LEVEL %1"), {{1, "NIVEL DE ENTREGA"}}), 1);
  Check(PatchTable(NarrowTable("Make 100% of the villagers happy"),
                   {{1, "Deixe 100% dos moradores felizes"}})
                .status == PatchStatus::kRebuilt,
        "literal percentage rejected");
}
void EntryFallbackTests() {
  const auto original = Fixture();
  const auto before = Read(original);
  const auto result =
      PatchTable(original, {{15132, "OPCOES"}, {19369, "<br>"}, {99, "Changed %1"}});
  Check(result.status == PatchStatus::kRebuilt && result.replacements == 1 &&
            result.rejected.size() == 2 && result.error.empty(),
        "bad entries discarded good translations");
  Check(result.rejected[0].id == 19369 && result.rejected[1].id == 99,
        "did not report every rejected ID");
  const auto rows = Read(result.bytes);
  Check(rows[0].text == "OPCOES", "valid translation lost");
  for (size_t index = 1; index < rows.size(); ++index) {
    Check(rows[index].encoded == before[index].encoded && rows[index].wide == before[index].wide &&
              rows[index].units == before[index].units &&
              rows[index].auxiliary == before[index].auxiliary,
          "rejected entry did not preserve its encoded original");
  }
  CheckRejected(PatchTable(original, {{15132, std::string("a\0b", 3)}}), 15132);
  CheckRejected(PatchTable(original, {{15132, "\xC3"}}), 15132);
  CheckRejected(PatchTable(original, {{15132, std::string(kMaxTextUnits + 1, 'a')}}), 15132);
  CheckRejected(PatchTable(NarrowTable("<orange>%1</orange>"), {{1, "<orange>%1"}}), 1);
}
void TypographyTests() {
  const auto original = NarrowTable("Original");
  // Exact Unicode values, independent of the codec's own decoding assumptions.
  for (const auto& [utf8, unit] : {std::pair{"\xE2\x80\x99", char16_t(0x2019)},
                                   {"\xE2\x80\x9C", char16_t(0x201C)},
                                   {"\xE2\x80\x9D", char16_t(0x201D)},
                                   {"\xE2\x80\x9E", char16_t(0x201E)},
                                   {"\xE2\x80\x93", char16_t(0x2013)},
                                   {"\xE2\x80\x94", char16_t(0x2014)},
                                   {"\xE2\x80\xA6", char16_t(0x2026)},
                                   {"\xC5\x93", char16_t(0x0153)},
                                   {"\xC2\x92", char16_t(0x0092)}}) {
    const auto result = PatchTable(original, {{1, utf8}});
    Check(result.status == PatchStatus::kRebuilt && result.rejected.empty(),
          "Unicode translation rejected");
    const auto row = Read(result.bytes)[0];
    Check(row.wide && row.units == 1 &&
              row.encoded == std::vector<uint8_t>{uint8_t(unit), uint8_t(unit >> 8), 0, 0},
          "Unicode punctuation must use exact UTF-16LE units");
    Check(row.text == utf8, "Unicode text did not round-trip");
  }
  const auto legacy = NarrowTable("\x85");
  Check(PatchTable(legacy, {{1, "\xE2\x80\xA6"}}).status == PatchStatus::kUnchanged,
        "unchanged original ellipsis must retain its original 0x85 byte");
  const auto changed = Read(PatchTable(legacy, {{1, "\xE2\x80\xA6 Novo"}}).bytes)[0];
  Check(changed.wide && changed.encoded[0] == 0x26 && changed.encoded[1] == 0x20,
        "changed legacy text must encode ellipsis as Unicode");
}
void PackageTests(const std::filesystem::path& root) {
  std::filesystem::create_directories(root / "pt-br-test");
  const auto folder = root / "pt-br-test";
  std::ofstream(folder / "mod.toml")
      << "[mod]\ntype='translation'\nbase_language='en'\nname='Test'\n";
  std::ofstream(folder / "textos.json") << R"({"15132":"OPTIONS TEST"})";
  Package package;
  std::string error;
  Check(LoadPackage(folder, package, error) && package.texts.size() == 1, "package rejected");
  std::ofstream(folder / "mod.toml")
      << "[mod]\ntype='translation'\nbase_language='en'\ntexts='../outside.json'\n";
  Check(!LoadPackage(folder, package, error), "external texts path accepted");
}
void RealTables(const std::filesystem::path& directory, const std::filesystem::path& artifacts) {
  Check(std::filesystem::is_directory(directory), "fixture directory does not exist");
  std::filesystem::create_directories(artifacts / "patched");
  unsigned files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != ".txu")
      continue;
    std::ifstream stream(entry.path(), std::ios::binary);
    const std::vector<uint8_t> source(std::istreambuf_iterator<char>(stream), {});
    const auto original = Read(source);
    const auto patched = PatchTable(source, {{15132,
                                              "OP\xC3\x87\xC3\x95"
                                              "ES"},
                                             {19369, "Legendas"},
                                             {21102, "JOGO PAUSADO"}});
    Check(patched.status != PatchStatus::kInvalid, "real table rejected");
    if (patched.status == PatchStatus::kRebuilt) {
      const auto rows = Read(patched.bytes);
      Check(rows.size() == original.size(), "real row count changed");
      for (size_t i = 0; i < rows.size(); ++i) {
        Check(rows[i].id == original[i].id && rows[i].auxiliary == original[i].auxiliary,
              "real ID/auxiliary changed");
        if (rows[i].id != 15132 && rows[i].id != 19369 && rows[i].id != 21102)
          Check(rows[i].encoded == original[i].encoded, "real untranslated bytes changed");
      }
      std::ofstream output(artifacts / "patched" / entry.path().filename(), std::ios::binary);
      output.write(reinterpret_cast<const char*>(patched.bytes.data()), patched.bytes.size());
    }
    ++files;
  }
  Check(files == 14, "expected all fourteen localized fixture tables");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2)
      throw std::runtime_error("Pass an artifacts directory for synthetic mod fixtures");
    JsonTests();
    TableTests();
    SubstitutionTests();
    EntryFallbackTests();
    TypographyTests();
    PackageTests(argv[1]);
    if (argc >= 3)
      RealTables(argv[2], argv[1]);
    std::cout << "translation PASS checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "translation FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
