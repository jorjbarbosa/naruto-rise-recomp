#pragma once

// The DLC packages refused for a module the SDK could only load with the
// devkit encryption key. The recompiled modules come from the retail ones, so
// a devkit build of one calls functions that do not exist and the game aborts
// at the start, every time; the game loads a single AI2C module and has no
// fallback when its load fails, so the package has to stay out of the game.
//
// The session that loads such a module records the installed packages that
// carry it (the module's name comes from the SDK's log, log_facts.h). The next
// start keeps them out: a package is not installed from the dlc folder, and an
// installed copy moves into the refused folder with its .header sidecar.
// Nothing is deleted; the player is told which file to remove.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace narutorise::dlc_refusal {
struct Refused {
  std::string package, module;
  std::string name;  // The DLC's display name, when known; the notice shows it.
};

// The installed packages (the folders under dlc_root) that carry a file named
// module, in any letter case.
std::vector<std::string> PackagesWithModule(const std::filesystem::path& dlc_root, std::string_view module);

// The record: {"refused": [{"package": …, "module": …, "name": …}]}. A
// missing or unreadable record refuses nothing.
std::vector<Refused> Read(const std::filesystem::path& record);
// Adds the packages not recorded yet, and a name to a recorded package that
// has none; false when the record cannot be written.
bool Add(const std::filesystem::path& record, const std::vector<Refused>& entries);

// The display name in an STFS package's header (the first locale's, UTF-16
// big-endian at 0x411), as UTF-8; empty when the file has none.
std::string PackageDisplayName(const std::filesystem::path& package);

// Moves an installed package's folder (dlc_root/package) and its .header
// sidecar into refused_dir, replacing an earlier refused copy; true when the
// package's folder was there.
bool MoveAside(const std::filesystem::path& dlc_root, const std::filesystem::path& header,
               const std::filesystem::path& refused_dir, std::string_view package);
}  // namespace narutorise::dlc_refusal
