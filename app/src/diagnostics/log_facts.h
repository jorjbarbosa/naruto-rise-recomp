#pragma once

// The facts the crash reports take from the SDK's log, read line by line
// (report_context.h installs the sink): a FATAL, whose abort follows the line,
// and a module the SDK loaded with the devkit encryption key, which the next
// "Registering module" line names. No runtime or logging dependency, so the
// recognition is testable on its own (tests/log_facts_test.cpp).
#include <optional>
#include <string>
#include <string_view>

namespace narutorise::log_facts {
// Addresses and hexadecimal values become "0x…" (or "…" without the prefix),
// so the same error at different addresses groups together.
std::string NormalizeFatal(std::string_view message);

struct Fact {
  std::optional<std::string> fatal;           // The message after "[FATAL] ", unchanged.
  std::optional<std::string> devkit_module;   // e.g. "ai2c@2.dll".
};

class Reader {
 public:
  // True when an info line can still bring a fact (a devkit load is pending);
  // otherwise only warnings and above need feeding.
  bool WantsInfo() const { return devkit_pending_; }
  Fact Feed(std::string_view line);

 private:
  bool devkit_pending_ = false;
};
}  // namespace narutorise::log_facts
