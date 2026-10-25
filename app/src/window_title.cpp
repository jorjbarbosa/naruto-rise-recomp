// narutorise - ReXGlue Recompiled Project
//
// The title of the game window (window_title.h). The version is the one the
// launcher shows: the narutorise.version file that the release package and
// the installer write next to the executable, or else the version this
// executable was built with (packaging/version.iss).

#include "window_title.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <rex/filesystem.h>

namespace narutorise {
namespace {

// A version is short and made of letters, digits, dots, dashes and pluses
// ("0.2.0-beta.1"); anything else in the file is not shown.
bool IsVersion(const std::string& text) {
  return !text.empty() && text.size() <= 64 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '-' || c == '+';
  });
}

}  // namespace

std::string Version() {
  std::ifstream file(rex::filesystem::GetExecutableFolder() / "narutorise.version");
  std::string line;
  if (file && std::getline(file, line)) {
    const auto first = line.find_first_not_of(" \t\r\n");
    const auto last = line.find_last_not_of(" \t\r\n");
    if (first != std::string::npos) line = line.substr(first, last - first + 1);
    if (IsVersion(line)) return line;
  }
#ifdef NARUTORISE_VERSION
  return NARUTORISE_VERSION;
#else
  return {};
#endif
}

std::string WindowTitle() {
  const std::string version = Version();
  return version.empty() ? "Naruto: Rise of a Ninja" : "Naruto: Rise of a Ninja - " + version;
}

}  // namespace narutorise
