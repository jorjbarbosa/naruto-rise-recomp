// Synthetic fixtures only: no game artwork is included in this test or build.
#include "menu/game_assets.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Bytes = std::vector<uint8_t>;
namespace assets = narutorise::menu::game_assets;
void Put(Bytes& b, size_t p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) b[p + i] = uint8_t(n >> (i * 8)); }
uint32_t Get(const Bytes& b, size_t p) { return b[p] | uint32_t(b[p+1]) << 8 | uint32_t(b[p+2]) << 16 | uint32_t(b[p+3]) << 24; }
void Check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
Bytes Header(uint32_t w, uint32_t h, const char* fourcc, size_t blocks_bytes) {
  Bytes b(128 + blocks_bytes);
  std::memcpy(b.data(), "DDS ", 4); Put(b, 4, 124); Put(b, 8, 0x81007);
  Put(b, 12, h); Put(b, 16, w); Put(b, 76, 32); Put(b, 80, 4);
  std::memcpy(b.data() + 84, fourcc, 4); Put(b, 108, 0x1000);
  return b;
}
// 8x4 BC1: the first block has four opaque colors (red > blue), indices 0..3 on
// its first row; the second has three colors (blue <= red) and index 3, which
// is transparent black, on its first pixel.
Bytes Bc1() {
  Bytes b = Header(8, 4, "DXT1", 16);
  b[128] = 0x00; b[129] = 0xf8; b[130] = 0x1f; b[131] = 0x00; b[132] = 0b11100100;
  b[136] = 0x1f; b[137] = 0x00; b[138] = 0x00; b[139] = 0xf8; b[140] = 0b01001011;
  return b;
}
Bytes Bc3() {
  Bytes b = Header(4, 4, "DXT5", 16);
  b[128] = 255; b[129] = 0; b[130] = 8;  // First pixel opaque, second transparent.
  b[136] = 0; b[137] = 0xf8;             // Red RGB565 endpoint, color index zero everywhere.
  return b;
}
Bytes Zlib(const Bytes& raw) {
  // RFC1950 + final, uncompressed DEFLATE blocks of at most 65535 bytes, then Adler32.
  Bytes b{0x78, 0x01};
  for (size_t at = 0; at < raw.size() || at == 0; at += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    b.insert(b.end(), {uint8_t(at + n >= raw.size()), uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t((~n) >> 8)});
    b.insert(b.end(), raw.begin() + at, raw.begin() + at + n);
    if (at + n >= raw.size()) break;
  }
  uint32_t a = 1, s = 0;
  for (auto v : raw) { a = (a + v) % 65521; s = (s + a) % 65521; }
  b.insert(b.end(), {uint8_t(s >> 8), uint8_t(s), uint8_t(a >> 8), uint8_t(a)});
  return b;
}
struct Record { std::string name; uint32_t key; Bytes dds; bool compressed = true; };
Bytes Sra(const std::vector<Record>& records) {
  std::vector<Bytes> payloads;
  size_t at = 24;
  for (const auto& r : records) { payloads.push_back(r.compressed ? Zlib(r.dds) : r.dds); at += payloads.back().size(); }
  const size_t index = at, names = index + 28 * records.size();
  Bytes b(names + 140 * records.size());
  std::memcpy(b.data(), "FRSA", 4); Put(b, 4, 1); Put(b, 8, uint32_t(index)); Put(b, 12, uint32_t(records.size()));
  Put(b, 16, uint32_t(names)); Put(b, 20, uint32_t(records.size()));
  at = 24;
  for (size_t i = 0; i < records.size(); ++i) {
    std::copy(payloads[i].begin(), payloads[i].end(), b.begin() + at);
    const size_t row = index + 28 * i, name = names + 140 * i;
    Put(b, row, 2); Put(b, row + 4, records[i].key); Put(b, row + 12, uint32_t(at));
    Put(b, row + 16, uint32_t(payloads[i].size())); Put(b, row + 20, uint32_t(records[i].dds.size())); Put(b, row + 24, 1);
    Put(b, name, 1); Put(b, name + 4, 2); Put(b, name + 8, records[i].key);
    std::memcpy(b.data() + name + 12, records[i].name.data(), records[i].name.size());
    at += payloads[i].size();
  }
  return b;
}
std::vector<Record> Good() { return {{"790006c2_I2", 7, Bc1()}, {"9e002bac_I105", 9, Bc3(), false}}; }
constexpr std::array<std::string_view, 2> kNames = {"790006c2_I2", "9e002bac_I105"};
std::optional<std::vector<assets::Image>> Parse(const Bytes& b, std::span<const std::string_view> names = kNames,
                                                size_t* bytes_read = nullptr) {
  return assets::ReadImages(b.size(), [&](uint64_t offset, std::span<uint8_t> out) {
    if (offset > b.size() || out.size() > b.size() - offset) return false;
    std::copy_n(b.data() + offset, out.size(), out.data());
    if (bytes_read) *bytes_read += out.size();
    return true;
  }, names);
}
bool Pixel(const assets::Image& image, uint32_t x, uint32_t y, std::array<uint8_t, 4> rgba) {
  return std::equal(rgba.begin(), rgba.end(), image.rgba.begin() + (size_t(y) * image.width + x) * 4);
}
void Pixels(const std::vector<assets::Image>& images) {
  Check(images.size() == 2, "count");
  const auto& bc1 = images[0];
  Check(bc1.width == 8 && bc1.height == 4 && bc1.rgba.size() == 128, "BC1 dimensions");
  Check(Pixel(bc1, 0, 0, {255, 0, 0, 255}) && Pixel(bc1, 1, 0, {0, 0, 255, 255}) &&
        Pixel(bc1, 2, 0, {170, 0, 85, 255}) && Pixel(bc1, 3, 0, {85, 0, 170, 255}), "BC1 four colors");
  Check(Pixel(bc1, 4, 0, {0, 0, 0, 0}) && Pixel(bc1, 5, 0, {127, 0, 127, 255}) &&
        Pixel(bc1, 6, 0, {0, 0, 255, 255}) && Pixel(bc1, 7, 0, {255, 0, 0, 255}), "BC1 three colors and transparency");
  const auto& bc3 = images[1];
  Check(bc3.width == 4 && bc3.height == 4, "BC3 dimensions");
  Check(Pixel(bc3, 0, 0, {255, 0, 0, 255}) && Pixel(bc3, 1, 0, {255, 0, 0, 0}), "BC3 color/alpha");
}
void Write(const std::filesystem::path& path, const Bytes& b) {
  std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  Check(bool(f), "write fixture");
}
Bytes Iso(const Bytes& file) {
  constexpr size_t sector = 2048;
  Bytes b(34 * sector + ((file.size() + sector - 1) / sector) * sector);
  std::memcpy(b.data() + 32 * sector, "MICROSOFT*XBOX*MEDIA", 20);
  Put(b, 32 * sector + 20, 33); Put(b, 32 * sector + 24, sector);
  std::fill(b.begin() + 33 * sector, b.begin() + 34 * sector, 255);
  const size_t at = 33 * sector;
  Put(b, at, 0); Put(b, at + 4, 34); Put(b, at + 8, uint32_t(file.size()));
  b[at + 12] = 0x20; b[at + 13] = 15;
  std::memcpy(b.data() + at + 14, "Naruto_clip.SRA", 15);
  std::copy(file.begin(), file.end(), b.begin() + 34 * sector);
  return b;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2 || std::filesystem::exists(argv[1])) return 2;
    const auto output = std::filesystem::path(argv[1]); std::filesystem::create_directories(output);
    const auto good = Sra(Good());
    size_t read = 0;
    auto images = Parse(good, kNames, &read); Check(bool(images), "two records"); Pixels(*images);
    Check(read == good.size(), "only header/catalog/index/payloads read");
    constexpr std::array<std::string_view, 2> reversed = {"9e002bac_I105", "790006c2_I2"};
    auto swapped = Parse(good, reversed);
    Check(swapped && (*swapped)[0].width == 4 && (*swapped)[1].width == 8, "requested order");
    constexpr std::array<std::string_view, 3> missing = {"790006c2_I2", "9e002bac_I105", "6e0003c4_I90"};
    Check(!Parse(good, missing), "all or nothing: missing name");
    Check(!Parse(good, std::span<const std::string_view>{}), "no names");
    std::vector<std::string_view> many(33, "790006c2_I2");
    Check(!Parse(good, many), "too many names");
    auto records = Good(); records.push_back({"790006c2_I2", 11, Bc1()});
    Check(!Parse(Sra(records)), "duplicate name");
    records = Good(); records[1].key = 7;
    Check(!Parse(Sra(records)), "duplicate key");
    auto bad = good; bad[24 + Zlib(Bc1()).size() - 1] ^= 1;
    Check(!Parse(bad), "all or nothing: zlib checksum of one record");
    for (auto [at, value] : std::array<std::pair<size_t, uint32_t>, 3>{{{84, 0x31545844 + 1}, {28, 2}, {12, 4096}}}) {
      records = Good(); Put(records[0].dds, at, value);
      Check(!Parse(Sra(records)), "invalid BC1 DDS");
    }
    records = Good(); records[0].dds.pop_back();
    Check(!Parse(Sra(records)), "BC1 size mismatch");
    for (size_t n : {size_t(0), size_t(23), good.size() - 1}) {
      Bytes cut(good.begin(), good.begin() + n); Check(!Parse(cut), "truncation");
    }
    Check(!assets::ReadImages(good.size(), [](uint64_t, std::span<uint8_t>) { return false; }, kNames), "read failure");
    const auto folder = output / std::filesystem::path(u8"jogo-ação"); std::filesystem::create_directories(folder);
    Write(folder / "Naruto_clip.SRA", good);
    auto local = assets::Load(folder, false, kNames); Check(bool(local), "folder"); Pixels(*local);
    Write(output / "synthetic.iso", Iso(good));
    auto disc = assets::Load(output / "synthetic.iso", true, kNames); Check(bool(disc), "ISO"); Pixels(*disc);
    Check(!assets::Load(output / "absent", false, kNames), "missing folder");
    Check(!assets::Load(output / "absent.iso", true, kNames), "missing ISO");
    // Optional real games: the 13 images of the in-game settings menu.
    constexpr std::array<std::pair<uint32_t, uint32_t>, 13> sizes = {{
        {872, 636}, {136, 724}, {36, 724}, {156, 120}, {344, 40}, {344, 40}, {48, 48}, {48, 48},
        {56, 36}, {56, 36}, {60, 24}, {60, 24}, {52, 52}}};
    for (int i = 2; i < argc; ++i) {
      const auto path = std::filesystem::path(argv[i]);
      auto real = assets::Load(path, !std::filesystem::is_directory(path), assets::kMenuImages);
      Check(real && real->size() == sizes.size(), "real menu images");
      for (size_t n = 0; n < sizes.size(); ++n) {
        const auto& image = (*real)[n];
        Check(image.width == sizes[n].first && image.height == sizes[n].second, "real image size");
        Check(std::any_of(image.rgba.begin(), image.rgba.end(), [](uint8_t b) { return b != 0; }), "real pixels");
        Write(output / ("real-" + std::to_string(i) + "-" + std::string(assets::kMenuImages[n]) + "-" +
                        std::to_string(image.width) + "x" + std::to_string(image.height) + ".rgba"), image.rgba);
      }
      std::cout << "PASS: real menu images " << path.string() << "\n";
    }
    std::cout << "PASS: bounded SRA, all-or-nothing lookup, BC1 four/three colors, BC3, malformed input, folder and ISO\n";
  } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
