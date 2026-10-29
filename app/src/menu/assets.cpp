// The files of the menus, embedded (assets.h). Each file of app/ui comes in
// through #embed (a Clang extension before C++26); CMake lists them as the
// object's dependencies, so a change rebuilds this unit.
#include "menu/assets.h"
#include <algorithm>
#include <array>
#include <cstdio>

namespace narutorise::menu {
namespace {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
// The bytes come as char: one above 127 narrows to its own uint8_t value.
#pragma clang diagnostic ignored "-Wc++11-narrowing"
constexpr uint8_t kRegular[] = {
#embed "../../ui/fonts/RobotoCondensed-Regular.ttf"
};
constexpr uint8_t kBold[] = {
#embed "../../ui/fonts/RobotoCondensed-Bold.ttf"
};
constexpr uint8_t kMedium[] = {
#embed "../../ui/fonts/RobotoCondensed-Medium.ttf"
};
constexpr uint8_t kRoboto[] = {
#embed "../../ui/fonts/Roboto-Regular.ttf"
};
constexpr uint8_t kTheme[] = {
#embed "../../ui/theme.rcss"
};
constexpr uint8_t kFps[] = {
#embed "../../ui/fps.rml"
};
constexpr uint8_t kSetup[] = {
#embed "../../ui/setup.rml"
};
constexpr uint8_t kSetupStyle[] = {
#embed "../../ui/setup.rcss"
};
constexpr uint8_t kDialog[] = {
#embed "../../ui/dialog.rml"
};
constexpr uint8_t kDialogStyle[] = {
#embed "../../ui/dialog.rcss"
};
constexpr uint8_t kPanel[] = {
#embed "../../ui/panel.rml"
};
constexpr uint8_t kPanelStyle[] = {
#embed "../../ui/panel.rcss"
};
constexpr uint8_t kControls[] = {
#embed "../../ui/controls.rml"
};
constexpr uint8_t kControlsStyle[] = {
#embed "../../ui/controls.rcss"
};
constexpr uint8_t kOverlay[] = {
#embed "../../ui/overlay.rml"
};
constexpr uint8_t kOverlayStyle[] = {
#embed "../../ui/overlay.rcss"
};
constexpr uint8_t kStatus[] = {
#embed "../../ui/status.rml"
};
constexpr uint8_t kStatusStyle[] = {
#embed "../../ui/status.rcss"
};
#pragma clang diagnostic pop

struct Entry {
  std::string_view path;
  std::span<const uint8_t> bytes;
};
constexpr std::array kAssets{
    Entry{"fonts/RobotoCondensed-Regular.ttf", kRegular},
    Entry{"fonts/RobotoCondensed-Bold.ttf", kBold},
    Entry{"fonts/RobotoCondensed-Medium.ttf", kMedium},
    Entry{"fonts/Roboto-Regular.ttf", kRoboto},
    Entry{"theme.rcss", kTheme},
    Entry{"fps.rml", kFps},
    Entry{"setup.rml", kSetup},
    Entry{"setup.rcss", kSetupStyle},
    Entry{"dialog.rml", kDialog},
    Entry{"dialog.rcss", kDialogStyle},
    Entry{"panel.rml", kPanel},
    Entry{"panel.rcss", kPanelStyle},
    Entry{"controls.rml", kControls},
    Entry{"controls.rcss", kControlsStyle},
    Entry{"overlay.rml", kOverlay},
    Entry{"overlay.rcss", kOverlayStyle},
    Entry{"status.rml", kStatus},
    Entry{"status.rcss", kStatusStyle},
};
}  // namespace

std::span<const uint8_t> Asset(std::string_view path) {
  while (!path.empty() && (path.front() == '/' || path.front() == '\\')) path.remove_prefix(1);
  for (const Entry& entry : kAssets)
    if (entry.path == path) return entry.bytes;
  return {};
}

AssetFiles::OpenFile* AssetFiles::File(Rml::FileHandle file) {
  return file && file <= files_.size() && files_[file - 1].open ? &files_[file - 1] : nullptr;
}

Rml::FileHandle AssetFiles::Open(const Rml::String& path) {
  const auto bytes = Asset(path);
  if (bytes.empty()) return 0;
  size_t slot = 0;
  while (slot < files_.size() && files_[slot].open) ++slot;
  if (slot == files_.size()) files_.emplace_back();
  files_[slot] = {bytes, 0, true};
  return slot + 1;
}

void AssetFiles::Close(Rml::FileHandle file) {
  if (OpenFile* f = File(file)) *f = {};
}

size_t AssetFiles::Read(void* buffer, size_t size, Rml::FileHandle file) {
  OpenFile* f = File(file);
  if (!f) return 0;
  const size_t count = std::min(size, f->bytes.size() - f->position);
  std::copy_n(f->bytes.data() + f->position, count, static_cast<uint8_t*>(buffer));
  f->position += count;
  return count;
}

bool AssetFiles::Seek(Rml::FileHandle file, long offset, int origin) {
  OpenFile* f = File(file);
  if (!f) return false;
  const long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? long(f->position) : long(f->bytes.size());
  const long position = base + offset;
  if (position < 0 || size_t(position) > f->bytes.size()) return false;
  f->position = size_t(position);
  return true;
}

size_t AssetFiles::Tell(Rml::FileHandle file) {
  const OpenFile* f = File(file);
  return f ? f->position : 0;
}

size_t AssetFiles::Length(Rml::FileHandle file) {
  const OpenFile* f = File(file);
  return f ? f->bytes.size() : 0;
}

}  // namespace narutorise::menu
