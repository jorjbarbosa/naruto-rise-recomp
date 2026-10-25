#include "folders.h"

#include <SDL3/SDL.h>

namespace launcher_ui {

std::string PathText(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

void OpenFolder(const std::filesystem::path& folder) {
  const auto path = folder.generic_u8string();
  // Encode reserved URI bytes so paths with spaces, accents or '#' open correctly.
  std::string url = "file:///";
  constexpr char hex[] = "0123456789ABCDEF";
  for (unsigned char c : path) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '/' || c == ':' || c == '-' || c == '_' || c == '.') {
      url += static_cast<char>(c);
    } else {
      url += '%'; url += hex[c >> 4]; url += hex[c & 15];
    }
  }
  SDL_OpenURL(url.c_str());
}

}  // namespace launcher_ui
