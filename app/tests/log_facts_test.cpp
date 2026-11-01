#include "diagnostics/log_facts.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace narutorise;
int main() {
  int checks = 0;
  const auto check = [&](bool value, const char* message) {
    ++checks; if (!value) throw std::runtime_error(message);
  };
  try {
    using log_facts::NormalizeFatal;
    check(NormalizeFatal("Call to invalid or unregistered function at guest address 0x88390B18") ==
              "Call to invalid or unregistered function at guest address 0x…", "0x addresses are replaced");
    check(NormalizeFatal("Call to invalid or unregistered function at guest address 0x82701EB0") ==
              NormalizeFatal("Call to invalid or unregistered function at guest address 0x88390B18"),
          "The same error at another address normalizes alike");
    check(NormalizeFatal("Execute(883A96A8): function not in function table") ==
              "Execute(…): function not in function table", "Bare eight-digit hexadecimal words are replaced");
    check(NormalizeFatal("BaseHeap::Release failed E0000000 state=0") == "BaseHeap::Release failed … state=0",
          "Small numbers stay");
    check(NormalizeFatal("check failed: deadline exceeded") == "check failed: deadline exceeded",
          "Words of hexadecimal letters without digits stay");
    check(NormalizeFatal("ZZ0x10 and 0xG") == "ZZ0x10 and 0xG", "0x inside a word or without digits stays");

    // A session of the 0.4.0-beta with the devkit DLC, line by line as the
    // SDK wrote it (the log attached to a NARUTORISE-7 event).
    log_facts::Reader reader;
    check(!reader.Feed("ContentManager lists 1 marketplace package(s)").fatal, "An ordinary line brings nothing");
    check(!reader.WantsInfo(), "No devkit load is pending at first");
    auto fact = reader.Feed("XEX load failed with code 3, trying with devkit encryption key...");
    check(!fact.devkit_module && reader.WantsInfo(), "The devkit load waits for the module's name");
    fact = reader.Feed("Function table initialized for module: code=88040000-883B5800, image=88000000-883D0000");
    check(!fact.devkit_module && reader.WantsInfo(), "Other info lines keep waiting");
    fact = reader.Feed("Registering module: ai2c@2.dll (code_base=88040000)");
    check(fact.devkit_module && *fact.devkit_module == "ai2c@2.dll", "The next registered module is the devkit one");
    check(!reader.WantsInfo(), "Only one module per devkit load");
    fact = reader.Feed("Registering module: ai2c@3.dll (code_base=88040000)");
    check(!fact.devkit_module, "A later module loaded with the retail key is not marked");
    check(!reader.Feed("Execute(883A96A8): function not in function table").fatal, "An error line is no FATAL");
    fact = reader.Feed("[FATAL] Call to invalid or unregistered function at guest address 0x88390B18");
    check(fact.fatal && *fact.fatal == "Call to invalid or unregistered function at guest address 0x88390B18",
          "The FATAL's message comes unchanged");
  } catch (const std::exception& error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
  std::cout << "log facts: " << checks << " checks passed\n";
  return 0;
}
