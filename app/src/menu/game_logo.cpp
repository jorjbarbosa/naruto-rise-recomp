#include "menu/game_logo.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <memory>
#include <string_view>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/disc_image_entry.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/system/xtypes.h>

// Reuse the project's existing MIT/Unlicense stb dependency, privately and
// with only its bounded zlib decoder. No new dependency or image is shipped.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_ZLIB
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#include <stb_image.h>

namespace narutorise::menu::game_logo {
namespace {
constexpr std::string_view kLogo = "6e0003ce_I15";
constexpr uint32_t kMaxEntries = 32768, kMaxImageBytes = 4 * 1024 * 1024;
uint32_t U32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
bool Range(uint64_t size, uint64_t offset, uint64_t length) {
  return offset <= size && length <= size - offset;
}
uint32_t Adler(std::span<const uint8_t> bytes) {
  uint32_t a = 1, b = 0;
  for (const auto byte : bytes) { a = (a + byte) % 65521; b = (b + a) % 65521; }
  return (b << 16) | a;
}
std::array<uint8_t, 3> Color565(uint32_t v) {
  return {uint8_t((v >> 11) * 255 / 31), uint8_t(((v >> 5) & 63) * 255 / 63), uint8_t((v & 31) * 255 / 31)};
}
std::optional<Image> Decode(std::span<const uint8_t> dds) {
  if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) || U32(dds.data() + 4) != 124 ||
      U32(dds.data() + 76) != 32 || U32(dds.data() + 80) != 4 ||
      std::memcmp(dds.data() + 84, "DXT5", 4) || U32(dds.data() + 24) != 0 ||
      U32(dds.data() + 28) > 1 || U32(dds.data() + 112) != 0) return {};
  const uint32_t h = U32(dds.data() + 12), w = U32(dds.data() + 16);
  if (!w || !h || w > 2048 || h > 2048 || uint64_t(w) * h * 4 > kMaxImageBytes) return {};
  const uint32_t columns = (w + 3) / 4, rows = (h + 3) / 4;
  if (dds.size() != 128 + uint64_t(columns) * rows * 16) return {};
  Image image{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
  const uint8_t* block = dds.data() + 128;
  for (uint32_t by = 0; by < rows; ++by) for (uint32_t bx = 0; bx < columns; ++bx, block += 16) {
    std::array<uint8_t, 8> alpha{block[0], block[1]};
    if (alpha[0] > alpha[1]) {
      for (unsigned i = 1; i < 7; ++i) alpha[i + 1] = uint8_t(((7 - i) * alpha[0] + i * alpha[1]) / 7);
    } else {
      for (unsigned i = 1; i < 5; ++i) alpha[i + 1] = uint8_t(((5 - i) * alpha[0] + i * alpha[1]) / 5);
      alpha[6] = 0; alpha[7] = 255;
    }
    uint64_t alpha_bits = 0;
    for (unsigned i = 0; i < 6; ++i) alpha_bits |= uint64_t(block[2 + i]) << (8 * i);
    std::array<std::array<uint8_t, 3>, 4> colors;
    colors[0] = Color565(uint32_t(block[8]) | uint32_t(block[9]) << 8);
    colors[1] = Color565(uint32_t(block[10]) | uint32_t(block[11]) << 8);
    // BC3 always uses four colors, regardless of endpoint ordering.
    for (unsigned c = 0; c < 3; ++c) {
      colors[2][c] = uint8_t((2 * colors[0][c] + colors[1][c]) / 3);
      colors[3][c] = uint8_t((colors[0][c] + 2 * colors[1][c]) / 3);
    }
    const uint32_t bits = U32(block + 12);
    for (unsigned i = 0; i < 16; ++i) {
      const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
      if (x >= w || y >= h) continue;
      uint8_t* pixel = image.rgba.data() + (size_t(y) * w + x) * 4;
      const auto& color = colors[(bits >> (2 * i)) & 3];
      std::copy(color.begin(), color.end(), pixel);
      pixel[3] = alpha[(alpha_bits >> (3 * i)) & 7];
    }
  }
  return image;
}
}  // namespace

std::optional<Image> ReadLogo(uint64_t file_size, const Read& read) {
  std::array<uint8_t, 24> header{};
  if (!read || !Range(file_size, 0, header.size()) || !read(0, header) ||
      std::memcmp(header.data(), "FRSA", 4) || U32(header.data() + 4) != 1) return {};
  const uint32_t index_at = U32(header.data() + 8), count = U32(header.data() + 12);
  const uint32_t names_at = U32(header.data() + 16), names_count = U32(header.data() + 20);
  if (!count || !names_count || count > kMaxEntries || names_count > kMaxEntries ||
      index_at < header.size() || names_at < header.size() ||
      !Range(file_size, index_at, uint64_t(count) * 28) ||
      !Range(file_size, names_at, uint64_t(names_count) * 140) ||
      uint64_t(index_at) + uint64_t(count) * 28 > names_at) return {};
  std::vector<uint8_t> names(size_t(names_count) * 140);
  if (!read(names_at, names)) return {};
  std::optional<uint32_t> key;
  for (size_t i = 0; i < names_count; ++i) {
    const uint8_t* name = names.data() + i * 140;
    if (U32(name) != 1 || U32(name + 4) != 2) continue;
    if (std::memcmp(name + 12, kLogo.data(), kLogo.size()) || name[12 + kLogo.size()] != 0) continue;
    if (key) return {};  // Ambiguous identity; do not guess which copy to display.
    key = U32(name + 8);
  }
  if (!key) return {};
  std::vector<uint8_t> index(size_t(count) * 28);
  if (!read(index_at, index)) return {};
  const uint8_t* entry = nullptr;
  for (size_t i = 0; i < count; ++i) {
    const auto* row = index.data() + i * 28;
    if (U32(row) != 2 || U32(row + 4) != *key) continue;
    if (entry) return {};
    entry = row;
  }
  if (!entry || U32(entry + 24) != 1) return {};
  const uint32_t offset = U32(entry + 12), packed_size = U32(entry + 16), size = U32(entry + 20);
  if (offset < header.size() || !packed_size || packed_size > kMaxImageBytes || size < 128 ||
      size > kMaxImageBytes || !Range(index_at, offset, packed_size)) return {};
  std::vector<uint8_t> packed(packed_size);
  if (!read(offset, packed)) return {};
  if (packed_size == size) return Decode(packed);
  if (packed_size < 6 || (packed[0] & 15) != 8 || (packed[0] >> 4) > 7 ||
      (packed[1] & 32) || ((unsigned(packed[0]) << 8) + packed[1]) % 31 != 0) return {};
  std::vector<uint8_t> unpacked(size);
  if (stbi_zlib_decode_buffer(reinterpret_cast<char*>(unpacked.data()), int(size),
                             reinterpret_cast<const char*>(packed.data()), int(packed_size)) != int(size)) return {};
  const auto* checksum = packed.data() + packed.size() - 4;
  const uint32_t expected = uint32_t(checksum[0]) << 24 | uint32_t(checksum[1]) << 16 |
                            uint32_t(checksum[2]) << 8 | checksum[3];
  if (Adler(unpacked) != expected) return {};
  return Decode(unpacked);
}

std::optional<Image> Load(const std::filesystem::path& game_root, bool disc_image) {
  try {
    if (game_root.empty()) return {};
    if (disc_image) {
      rex::filesystem::DiscImageDevice device("\\Device\\PrelaunchArt", game_root);
      if (!device.Initialize()) return {};
      auto* entry = device.ResolvePath("Naruto_clip.SRA");
      if (!entry || (entry->attributes() & rex::filesystem::kFileAttributeDirectory)) return {};
      // The directory can describe a file extending beyond a truncated ISO.
      // Check the mapped extent before DiscImageFile copies any of its bytes.
      const auto* disc_entry = static_cast<const rex::filesystem::DiscImageEntry*>(entry);
      if (!Range(disc_entry->mmap()->size(), disc_entry->data_offset(), disc_entry->data_size())) return {};
      rex::filesystem::File* raw_file = nullptr;
      if (XFAILED(entry->Open(rex::filesystem::FileAccess::kFileReadData, &raw_file)) || !raw_file) return {};
      const auto destroy = [](rex::filesystem::File* file) { file->Destroy(); };
      std::unique_ptr<rex::filesystem::File, decltype(destroy)> file(raw_file, destroy);
      return ReadLogo(entry->size(), [&](uint64_t offset, std::span<uint8_t> bytes) {
        size_t got = 0;
        return XSUCCEEDED(file->ReadSync(bytes, size_t(offset), &got)) && got == bytes.size();
      });
    }
    // The application normalizes default.xex to its containing directory.
    const auto path = game_root / "Naruto_clip.SRA";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto end = file.tellg();
    if (end < 0) return {};
    return ReadLogo(uint64_t(end), [&](uint64_t offset, std::span<uint8_t> bytes) {
      file.seekg(std::streamoff(offset));
      return bool(file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())));
    });
  } catch (...) {
    // Optional artwork must never turn a usable game into a failed launch.
    return {};
  }
}
}  // namespace narutorise::menu::game_logo
