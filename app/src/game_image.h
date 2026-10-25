// narutorise - ReXGlue Recompiled Project
//
// Which game a game data root holds. The root is a disc image read in place
// (the SDK mounts it with DiscImageDevice) or a folder of extracted files. The
// recompiled code only runs with the default.xex it was generated from, so the
// identity is the Title ID plus a digest of that file, not just the Title ID.
// Ready also requires the core engine, shader and data archives to be present.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace narutorise::game_image {

enum class Status {
  kReady,         // Compatible default.xex with the required game data files.
  kOtherVersion,  // Naruto: Rise of a Ninja, another default.xex.
  kOtherGame,     // An Xbox 360 disc or folder of another title.
  kNotXbox360,    // Neither an Xbox 360 disc image nor a folder with default.xex.
  kUnreadable,    // Missing, or it could not be read.
  kIncomplete,    // Compatible default.xex, but required game data is missing.
};

struct Identity {
  Status status = Status::kUnreadable;
  bool disc_image = false;
  uint32_t title_id = 0;    // From the XEX execution information; 0 when absent.
  std::string xex_digest;   // XXH3-128 of default.xex (rex::hash_bytes).
};

// root: a disc image file, a folder with default.xex, or that default.xex.
Identity Identify(const std::filesystem::path& root);

// Prefer a playable installation over a nearby code-generation input folder.
std::filesystem::path FindNearbyGame(const std::filesystem::path& executable_folder);

const char* StatusName(Status status);

}  // namespace narutorise::game_image
