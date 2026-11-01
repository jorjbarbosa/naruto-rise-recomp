// The facts of the SDK's log (log_facts.h).
#include "diagnostics/log_facts.h"

namespace narutorise::log_facts {
namespace {
// The lines, as the SDK writes them (function_dispatcher.cpp, xex_module.cpp,
// logging/assert.h).
constexpr std::string_view kFatal = "[FATAL] ";
constexpr std::string_view kDevkitLoad = "XEX load failed with code ";
constexpr std::string_view kDevkitKey = "devkit encryption key";
constexpr std::string_view kRegistering = "Registering module: ";

bool IsHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool IsWord(char c) { return IsHex(c) || (c >= 'g' && c <= 'z') || (c >= 'G' && c <= 'Z') || c == '_'; }
}  // namespace

std::string NormalizeFatal(std::string_view message) {
  std::string out;
  out.reserve(message.size());
  for (size_t i = 0; i < message.size();) {
    const bool word_start = i == 0 || !IsWord(message[i - 1]);
    // 0x followed by hexadecimal digits.
    if (word_start && i + 2 < message.size() && message[i] == '0' && (message[i + 1] == 'x' || message[i + 1] == 'X') &&
        IsHex(message[i + 2])) {
      size_t end = i + 2;
      while (end < message.size() && IsHex(message[end])) ++end;
      out += "0x…";
      i = end;
      continue;
    }
    // A bare word of eight or more hexadecimal digits with a digit in it
    // (a guest address as the SDK prints it, 883A96A8).
    if (word_start && IsHex(message[i])) {
      size_t end = i;
      bool digit = false;
      while (end < message.size() && IsHex(message[end])) digit |= message[end++] <= '9';
      if (end - i >= 8 && digit && (end == message.size() || !IsWord(message[end]))) {
        out += "…";
        i = end;
        continue;
      }
    }
    out += message[i++];
  }
  return out;
}

Fact Reader::Feed(std::string_view line) {
  Fact fact;
  if (line.starts_with(kFatal)) {
    fact.fatal = std::string(line.substr(kFatal.size()));
  } else if (line.starts_with(kDevkitLoad) && line.find(kDevkitKey) != std::string_view::npos) {
    devkit_pending_ = true;
  } else if (devkit_pending_ && line.starts_with(kRegistering)) {
    devkit_pending_ = false;
    std::string_view module = line.substr(kRegistering.size());
    module = module.substr(0, module.find(" ("));
    if (!module.empty()) fact.devkit_module = std::string(module);
  }
  return fact;
}
}  // namespace narutorise::log_facts
