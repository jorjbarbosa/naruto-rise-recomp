#include "diagnostics/incident.h"
#include "gpu/backend/failure_channel.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> arguments;
  for (int i = 0; i < argc; ++i) {
    const auto bytes = std::filesystem::path(argv[i]).u8string();
    arguments.emplace_back(bytes.begin(), bytes.end());
  }
#else
int main(int argc, char** argv) {
  std::vector<std::string> arguments(argv, argv + argc);
#endif
  const auto executable = std::filesystem::absolute(std::filesystem::path(std::u8string(arguments[0].begin(), arguments[0].end())));
  return narutorise::incident::RunSupervised(arguments, executable, [](auto& args) {
    using namespace narutorise::incident;
    Event("fixture", "begin");
    SetMetadata("build", "fixture");
    std::string mode = "normal";
    for (const auto& arg : args) if (arg.starts_with("--fixture=")) mode = arg.substr(10);
    if (mode == "exit") std::_Exit(77);
    if (mode == "guest") {
      std::array<uint64_t, 32> registers{}; registers[1] = 0x7010fc00; registers[13] = 0x80010000;
      GuestFault(0x1111, 0x1234, 0, 4, 0x100000000ull, registers, 0x82130000, 0, 1);
      GuestFault(0x2222, 0x5678, 0, 4, 0x100000000ull, registers, 0x82130004, 0, 2);
    }
    if (mode == "failure" || mode == "blocked") {
      gpu::FailureChannel channel;
      channel.Retain({1, "original-gpu-operation", "{\"hresult\":2289696773}"});
      channel.Retain({2, "cleanup-error", "must-not-replace-original"});
      if (mode == "failure") std::_Exit(88);
    }
    if (mode == "snapshot" || mode == "blocked") {
      RequestSnapshot();
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    if (mode == "stderr") for (int i = 0; i < 50000; ++i) std::fputs("pipe-must-be-drained-continuously-even-after-the-retention-limit\n", stderr);
    if (mode == "concurrent") {
      std::vector<std::thread> threads;
      for (int n = 0; n < 8; ++n) threads.emplace_back([n] {
        for (int i = 0; i < 500; ++i) { Event("worker", "progress", std::to_string(n)); Progress(Role::kCompiler); }
      });
      for (auto& thread : threads) thread.join();
    }
    Event("fixture", "end");
    ExpectedExit("fixture-normal");
    return 0;
  });
}
