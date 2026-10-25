// Menu strings extend the launcher's seven-language table. RML uses @keys;
// settings reuse TextId without duplicating descriptions or language logic.
#pragma once
#include "../menu/localization.h"
#include <RmlUi/Core/SystemInterface.h>
#include <string>
#include <string_view>
namespace narutorise::menu {
const char* Text(std::string_view key);
class TextSystem final : public Rml::SystemInterface {
 public:
  int TranslateString(Rml::String& translated, const Rml::String& input) override;
};
}  // namespace narutorise::menu
