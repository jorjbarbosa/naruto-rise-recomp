// The files of the menus (app/ui), embedded in the executable: the fonts and
// the documents with their style sheets, by their path under ui/ (as
// "fonts/RobotoCondensed-Regular.ttf"). RmlUi reads them through AssetFiles;
// a path that is not embedded does not open.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <RmlUi/Core/FileInterface.h>

namespace narutorise::menu {

// The bytes of an embedded file; empty when there is none at path.
std::span<const uint8_t> Asset(std::string_view path);

class AssetFiles final : public Rml::FileInterface {
 public:
  Rml::FileHandle Open(const Rml::String& path) override;
  void Close(Rml::FileHandle file) override;
  size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
  bool Seek(Rml::FileHandle file, long offset, int origin) override;
  size_t Tell(Rml::FileHandle file) override;
  size_t Length(Rml::FileHandle file) override;

 private:
  struct OpenFile {
    std::span<const uint8_t> bytes;
    size_t position = 0;
    bool open = false;
  };
  std::vector<OpenFile> files_;  // A handle is its index + 1.
  OpenFile* File(Rml::FileHandle file);
};

}  // namespace narutorise::menu
