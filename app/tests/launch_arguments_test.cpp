#include "launch_arguments.h"
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
namespace fs = std::filesystem;
using namespace narutorise;
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  int checks = 0;
  const auto check = [&](bool value, const char* message) {
    ++checks; if (!value) throw std::runtime_error(message);
  };
  try {
    const auto root = fs::absolute(argv[1]);
    data::DataPaths paths;
    paths.data_root = root / fs::path(u8"data 日本語");
    paths.user_data_root = paths.data_root / "user";
    paths.cache_root = paths.data_root / "cache";
    check(launch::StorageArguments(paths).empty(), "Derived default roots must not become overrides");
    for (auto source : {data::Source::kUserDescriptor, data::Source::kPortableDescriptor}) {
      paths.data_source = source;
      paths.user_source = paths.cache_source = data::Source::kConfig;
      check(launch::StorageArguments(paths).empty(), "Persisted selection and TOML stay authoritative after restart");
    }
    for (auto source : {data::Source::kCommandLine, data::Source::kEnvironment}) {
      paths.data_source = source;
      paths.user_source = paths.cache_source = data::Source::kDefault;
      auto args = launch::StorageArguments(paths);
      check(args.size() == 1 && args.front() == "--data_root=" + launch::Utf8(paths.data_root),
            "General override is preserved without a derived guest override");
      paths.user_source = source; paths.cache_source = source;
      paths.user_data_root = root / "explicit guest";
      paths.cache_root = root / "explicit cache";
      args = launch::StorageArguments(paths);
      check(args.size() == 3 && args[1] == "--user_data_root=" + launch::Utf8(paths.user_data_root) &&
            args[2] == "--cache_root=" + launch::Utf8(paths.cache_root), "Only explicit storage flags survive");
#ifdef _WIN32
      // Parse with the OS, including Unicode, quotes, spaces and trailing slashes.
      args.push_back("--log_file=C:\\quoted \\\"name\\\"\\");
      auto command = L"game.exe " + launch::CommandLine(args);
      int count = 0; auto parsed = CommandLineToArgvW(command.c_str(), &count);
      bool same = parsed && count == static_cast<int>(args.size()) + 1;
      for (int i = 1; same && i < count; ++i)
        same = fs::path(parsed[i]).u8string() == std::u8string(args[i-1].begin(), args[i-1].end());
      if (parsed) LocalFree(parsed);
      check(same, "Windows command line round-trips every literal argument");
#endif
    }
    std::cout << "PASS: " << checks << " checks; transient storage sources, no derived override, literal argv\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1;
  }
}
