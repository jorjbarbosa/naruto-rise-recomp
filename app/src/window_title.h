// narutorise - ReXGlue Recompiled Project
//
// The title of the game window (window_title.cpp): the game's name and the
// version of this copy, in place of the SDK's name and build stamp.

#pragma once

#include <string>

namespace narutorise {
// The version of this copy ("0.3.0-beta"), or empty when it is not known.
std::string Version();
// "Naruto: Rise of a Ninja - <version>", or the name alone when the version is not known.
std::string WindowTitle();
}  // namespace narutorise
