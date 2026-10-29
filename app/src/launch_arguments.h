// Session storage arguments shared by shell links and process restarts.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "data_paths.h"

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

inline bool Explicit(data::Source source) {
  return source == data::Source::kCommandLine || source == data::Source::kEnvironment;
}
inline std::string Utf8(const std::filesystem::path& path) {
  const auto value = path.u8string(); return {value.begin(), value.end()};
}
inline std::vector<std::string> StorageArguments(const data::DataPaths& paths) {
  std::vector<std::string> result;
  const auto add = [&](const char* flag, const std::filesystem::path& root, data::Source source) {
    if (Explicit(source)) result.push_back(std::string("--") + flag + "=" + Utf8(root));
  };
  add("data_root", paths.data_root, paths.data_source);
  add("user_data_root", paths.user_data_root, paths.user_source);
  add("cache_root", paths.cache_root, paths.cache_source);
  return result;
}
#ifdef _WIN32
inline std::wstring CommandLine(const std::vector<std::string>& arguments) {
  std::wstring result;
  for (const auto& argument : arguments) {
    if (!result.empty()) result += L" ";
    result += QuoteArgument(std::filesystem::path(std::u8string(argument.begin(), argument.end())).wstring());
  }
  return result;
}
#endif
}  // namespace narutorise::launch
