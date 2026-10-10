#include "localization.h"

#include <cstring>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<std::string> Placeholders(const char* text) {
  const std::string value(text);
  const std::regex pattern(R"(%(?:\.\d+)?[suf])");
  std::vector<std::string> result;
  for (std::sregex_iterator it(value.begin(), value.end(), pattern), end; it != end; ++it) {
    result.push_back(it->str());
  }
  return result;
}

}  // namespace

int main() {
  try {
    Require(Localization::GetLanguageCode() == "en", "Default interface language must be English");
    const char* expected[] = {"en", "pt_BR", "fr", "de", "es", "it", "ru"};
    const auto& options = Localization::GetLanguages();
    Require(options.size() == std::size(expected), "Expected seven interface languages");
    for (size_t i = 0; i < options.size(); ++i) {
      Require(std::strcmp(options[i].code, expected[i]) == 0, "Incorrect language order");
      Localization::SetLanguageCode(options[i].code);
      Require(Localization::GetLanguage() == options[i].language, "Code resolved to wrong language");
      Require(Localization::GetLanguageCode() == options[i].code, "Language code did not round-trip");
      Require(options[i].name[0] != '\0', "Language option has no display name");
      for (int id = 0; id < static_cast<int>(TextId::Count); ++id) {
        const auto text_id = static_cast<TextId>(id);
        Localization::SetLanguage(Localization::Language::English);
        const auto placeholders = Placeholders(Tr(text_id));
        Localization::SetLanguage(options[i].language);
        Require(Tr(text_id)[0] != '\0', "Translation is missing");
        Require(Placeholders(Tr(text_id)) == placeholders, "Translation changed printf placeholders");
      }
    }
    for (const auto* alias : {"pt", "pt-BR", "pt_BR"}) {
      Localization::SetLanguageCode(alias);
      Require(Localization::GetLanguageCode() == "pt_BR", "Portuguese compatibility alias failed");
    }
    Localization::SetLanguageCode("unsupported");
    Require(Localization::GetLanguageCode() == "en", "Unsupported language must fall back to English");
    Localization::SetLanguage(Localization::Language::Count);
    Require(Localization::GetLanguageCode() == "en", "Invalid language enum must fall back to English");
    Require(std::strcmp(Tr(TextId::Count), "") == 0, "Invalid text ID must return empty text");
    std::cout << "Seven languages complete; order and codes correct; placeholders preserved; English default\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
