#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// Single-file Setup support. The release pipeline appends the distribution
// payload to the installer .exe and closes the file with a trailer:
//   [payload bytes][int64 payload_size]["NARUTO_RISE_PAYLOAD"]
// At startup the installer unpacks the payload to a temp folder. When there is
// no embedded payload (development runs), paths resolve from the folder the
// installer runs from.
namespace installer_payload {

constexpr char kMarker[] = "NARUTO_RISE_PAYLOAD";
constexpr size_t kMarkerSize = sizeof(kMarker) - 1;  // 19 bytes

// Unpacks the embedded payload (if any) to a temp folder. Returns false only
// on a corrupt trailer; a missing payload is not an error.
bool ExtractEmbedded(std::string& error);

bool HasEmbedded();

// Resolves a payload-relative path: embedded temp dir first, then the
// installer's own folder.
std::filesystem::path Resolve(const std::filesystem::path& relative);

// Directory the installer executable runs from.
std::filesystem::path InstallerDir();

// Full path of the running installer executable (may be empty off-Windows).
std::filesystem::path OwnExePath();

// True when a usable payload exists (embedded, or port files next to the exe).
bool Available();

}  // namespace installer_payload
