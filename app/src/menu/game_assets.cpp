#include "menu/game_assets.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <memory>
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

namespace narutorise::menu::game_assets {
namespace {
constexpr uint32_t kMaxEntries = 32768, kMaxImageBytes = 4 * 1024 * 1024, kMaxImages = 32;
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
      U32(dds.data() + 76) != 32 || U32(dds.data() + 80) != 4 || U32(dds.data() + 24) != 0 ||
      U32(dds.data() + 28) > 1 || U32(dds.data() + 112) != 0) return {};
  const bool bc1 = !std::memcmp(dds.data() + 84, "DXT1", 4);
  if (!bc1 && std::memcmp(dds.data() + 84, "DXT5", 4)) return {};
  const uint32_t h = U32(dds.data() + 12), w = U32(dds.data() + 16), block_size = bc1 ? 8 : 16;
  if (!w || !h || w > 2048 || h > 2048 || uint64_t(w) * h * 4 > kMaxImageBytes) return {};
  const uint32_t columns = (w + 3) / 4, rows = (h + 3) / 4;
  if (dds.size() != 128 + uint64_t(columns) * rows * block_size) return {};
  Image image{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
  const uint8_t* block = dds.data() + 128;
  for (uint32_t by = 0; by < rows; ++by) for (uint32_t bx = 0; bx < columns; ++bx, block += block_size) {
    std::array<uint8_t, 8> alpha{255, 255, 255, 255, 255, 255, 255, 255};
    uint64_t alpha_bits = 0;
    const uint8_t* color_block = block;
    if (!bc1) {
      alpha[0] = block[0]; alpha[1] = block[1];
      if (alpha[0] > alpha[1]) {
        for (unsigned i = 1; i < 7; ++i) alpha[i + 1] = uint8_t(((7 - i) * alpha[0] + i * alpha[1]) / 7);
      } else {
        for (unsigned i = 1; i < 5; ++i) alpha[i + 1] = uint8_t(((5 - i) * alpha[0] + i * alpha[1]) / 5);
        alpha[6] = 0; alpha[7] = 255;
      }
      for (unsigned i = 0; i < 6; ++i) alpha_bits |= uint64_t(block[2 + i]) << (8 * i);
      color_block = block + 8;
    }
    const uint32_t c0 = uint32_t(color_block[0]) | uint32_t(color_block[1]) << 8;
    const uint32_t c1 = uint32_t(color_block[2]) | uint32_t(color_block[3]) << 8;
    std::array<std::array<uint8_t, 3>, 4> colors{Color565(c0), Color565(c1)};
    // BC3 always uses four colors; BC1 uses three and transparent black when c0 <= c1.
    const bool four = !bc1 || c0 > c1;
    for (unsigned c = 0; c < 3; ++c) {
      if (four) {
        colors[2][c] = uint8_t((2 * colors[0][c] + colors[1][c]) / 3);
        colors[3][c] = uint8_t((colors[0][c] + 2 * colors[1][c]) / 3);
      } else {
        colors[2][c] = uint8_t((colors[0][c] + colors[1][c]) / 2);
        colors[3][c] = 0;
      }
    }
    const uint32_t bits = U32(color_block + 4);
    for (unsigned i = 0; i < 16; ++i) {
      const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
      if (x >= w || y >= h) continue;
      uint8_t* pixel = image.rgba.data() + (size_t(y) * w + x) * 4;
      const unsigned index = (bits >> (2 * i)) & 3;
      std::copy(colors[index].begin(), colors[index].end(), pixel);
      pixel[3] = bc1 ? uint8_t(four || index != 3 ? 255 : 0) : alpha[(alpha_bits >> (3 * i)) & 7];
    }
  }
  return image;
}
std::optional<Image> Unpack(const Read& read, uint64_t index_at, const uint8_t* entry) {
  if (U32(entry + 24) != 1) return {};
  const uint32_t offset = U32(entry + 12), packed_size = U32(entry + 16), size = U32(entry + 20);
  if (offset < 24 || !packed_size || packed_size > kMaxImageBytes || size < 128 ||
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
}  // namespace

std::optional<std::vector<Image>> ReadImages(uint64_t file_size, const Read& read,
                                             std::span<const std::string_view> names) {
  std::array<uint8_t, 24> header{};
  if (!read || names.empty() || names.size() > kMaxImages || !Range(file_size, 0, header.size()) ||
      !read(0, header) || std::memcmp(header.data(), "FRSA", 4) || U32(header.data() + 4) != 1) return {};
  const uint32_t index_at = U32(header.data() + 8), count = U32(header.data() + 12);
  const uint32_t names_at = U32(header.data() + 16), names_count = U32(header.data() + 20);
  if (!count || !names_count || count > kMaxEntries || names_count > kMaxEntries ||
      index_at < header.size() || names_at < header.size() ||
      !Range(file_size, index_at, uint64_t(count) * 28) ||
      !Range(file_size, names_at, uint64_t(names_count) * 140) ||
      uint64_t(index_at) + uint64_t(count) * 28 > names_at) return {};
  std::vector<uint8_t> catalog(size_t(names_count) * 140);
  if (!read(names_at, catalog)) return {};
  std::vector<std::optional<uint32_t>> keys(names.size());
  for (size_t i = 0; i < names_count; ++i) {
    const uint8_t* name = catalog.data() + i * 140;
    if (U32(name) != 1 || U32(name + 4) != 2) continue;
    for (size_t n = 0; n < names.size(); ++n) {
      const auto& wanted = names[n];
      if (wanted.empty() || wanted.size() >= 128 || std::memcmp(name + 12, wanted.data(), wanted.size()) ||
          name[12 + wanted.size()] != 0) continue;
      if (keys[n]) return {};  // Ambiguous identity; do not guess which copy to use.
      keys[n] = U32(name + 8);
    }
  }
  if (std::any_of(keys.begin(), keys.end(), [](const auto& key) { return !key; })) return {};
  std::vector<uint8_t> index(size_t(count) * 28);
  if (!read(index_at, index)) return {};
  std::vector<const uint8_t*> entries(names.size());
  for (size_t i = 0; i < count; ++i) {
    const auto* row = index.data() + i * 28;
    if (U32(row) != 2) continue;
    for (size_t n = 0; n < names.size(); ++n) {
      if (U32(row + 4) != *keys[n]) continue;
      if (entries[n]) return {};
      entries[n] = row;
    }
  }
  std::vector<Image> images;
  images.reserve(names.size());
  for (const auto* entry : entries) {
    if (!entry) return {};
    auto image = Unpack(read, index_at, entry);
    if (!image) return {};
    images.push_back(std::move(*image));
  }
  return images;
}

std::optional<std::vector<Image>> Load(const std::filesystem::path& game_root, bool disc_image,
                                       std::span<const std::string_view> names) {
  try {
    if (game_root.empty()) return {};
    if (disc_image) {
      rex::filesystem::DiscImageDevice device("\\Device\\MenuArt", game_root);
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
      return ReadImages(entry->size(), [&](uint64_t offset, std::span<uint8_t> bytes) {
        size_t got = 0;
        return XSUCCEEDED(file->ReadSync(bytes, size_t(offset), &got)) && got == bytes.size();
      }, names);
    }
    // The application normalizes default.xex to its containing directory.
    const auto path = game_root / "Naruto_clip.SRA";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto end = file.tellg();
    if (end < 0) return {};
    return ReadImages(uint64_t(end), [&](uint64_t offset, std::span<uint8_t> bytes) {
      file.seekg(std::streamoff(offset));
      return bool(file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())));
    }, names);
  } catch (...) {
    // Optional artwork must never turn a usable game into a failed launch.
    return {};
  }
}
}  // namespace narutorise::menu::game_assets
