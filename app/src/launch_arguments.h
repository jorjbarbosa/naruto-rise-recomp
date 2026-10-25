// Windows argv quoting shared by shell links and process restarts.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace narutorise::launch {
inline std::wstring QuoteArgument(std::wstring_view value) {
  std::wstring result(1, L'"');
  size_t slashes = 0;
  for (const auto c : value) {
    if (c == L'\\') { ++slashes; continue; }
    result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    result += c;
    slashes = 0;
  }
  result.append(slashes * 2, L'\\');
  result += L'"';
  return result;
}

inline std::wstring UserDataArgument(const std::filesystem::path& root) {
  return QuoteArgument(L"--user_data_root=" + std::filesystem::absolute(root).lexically_normal().wstring());
}
}  // namespace narutorise::launch
