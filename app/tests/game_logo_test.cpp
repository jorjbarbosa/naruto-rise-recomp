// Synthetic fixtures only: no game artwork is included in this test or build.
#include "menu/game_logo.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using Bytes = std::vector<uint8_t>;
void Put(Bytes& b, size_t p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) b[p + i] = uint8_t(n >> (i * 8)); }
uint32_t Get(const Bytes& b, size_t p) { return b[p] | uint32_t(b[p+1]) << 8 | uint32_t(b[p+2]) << 16 | uint32_t(b[p+3]) << 24; }
void Check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
Bytes Dds() {
  Bytes b(144);
  std::memcpy(b.data(), "DDS ", 4); Put(b, 4, 124); Put(b, 8, 0x81007);
  Put(b, 12, 4); Put(b, 16, 4); Put(b, 20, 16); Put(b, 76, 32); Put(b, 80, 4);
  std::memcpy(b.data() + 84, "DXT5", 4); Put(b, 108, 0x1000);
  b[128] = 255; b[129] = 0; b[130] = 8; // First pixel opaque, second transparent.
  b[136] = 0; b[137] = 0xf8; // Red RGB565 endpoint, color index zero everywhere.
  return b;
}
Bytes Zlib(const Bytes& raw) {
  // RFC1950 + a final, uncompressed DEFLATE block, followed by Adler32.
  Bytes b{0x78, 0x01, 0x01, uint8_t(raw.size()), uint8_t(raw.size() >> 8),
          uint8_t(~raw.size()), uint8_t((~raw.size()) >> 8)};
  b.insert(b.end(), raw.begin(), raw.end());
  uint32_t a = 1, s = 0;
  for (auto v : raw) { a = (a + v) % 65521; s = (s + a) % 65521; }
  b.insert(b.end(), {uint8_t(s >> 8), uint8_t(s), uint8_t(a >> 8), uint8_t(a)});
  return b;
}
Bytes Sra(const Bytes& dds = Dds(), bool compressed = true) {
  const Bytes payload = compressed ? Zlib(dds) : dds;
  const size_t index = 24 + payload.size(), names = index + 28;
  Bytes b(names + 140);
  std::memcpy(b.data(), "FRSA", 4); Put(b, 4, 1); Put(b, 8, uint32_t(index)); Put(b, 12, 1);
  Put(b, 16, uint32_t(names)); Put(b, 20, 1);
  std::copy(payload.begin(), payload.end(), b.begin() + 24);
  Put(b, index, 2); Put(b, index + 4, 123); Put(b, index + 12, 24);
  Put(b, index + 16, uint32_t(payload.size())); Put(b, index + 20, uint32_t(dds.size())); Put(b, index + 24, 1);
  Put(b, names, 1); Put(b, names + 4, 2); Put(b, names + 8, 123);
  std::memcpy(b.data() + names + 12, "6e0003ce_I15", 12);
  return b;
}
std::optional<narutorise::menu::game_logo::Image> Parse(const Bytes& b, size_t* bytes_read = nullptr) {
  return narutorise::menu::game_logo::ReadLogo(b.size(), [&](uint64_t offset, std::span<uint8_t> out) {
    if (offset > b.size() || out.size() > b.size() - offset) return false;
    std::copy_n(b.data() + offset, out.size(), out.data());
    if (bytes_read) *bytes_read += out.size();
    return true;
  });
}
void Pixels(const narutorise::menu::game_logo::Image& image) {
  Check(image.width == 4 && image.height == 4 && image.rgba.size() == 64, "dimensions");
  Check(std::equal(image.rgba.begin(), image.rgba.begin() + 8,
                   std::array<uint8_t, 8>{255,0,0,255,255,0,0,0}.begin()), "BC3 color/alpha");
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
    auto good = Sra(); size_t read = 0;
    auto image = Parse(good, &read); Check(bool(image), "compressed SRA"); Pixels(*image);
    Check(read == good.size(), "only header/catalog/payload read");
    auto plain = Parse(Sra(Dds(), false)); Check(bool(plain), "uncompressed SRA"); Pixels(*plain);
    for (size_t n : {size_t(0), size_t(23), good.size() - 1}) {
      Bytes bad(good.begin(), good.begin() + n); Check(!Parse(bad), "truncation");
    }
    const size_t index = Get(good, 8), names = Get(good, 16);
    for (auto [at, value] : std::array<std::pair<size_t,uint32_t>, 12>{{
         {4,2}, {12,0xffffffff}, {20,0xffffffff}, {8,0xfffffff0}, {16,0xfffffff0},
         {index+12,0xfffffff0}, {index+16,0xffffffff}, {index+20,0xffffffff},
         {index+24,2}, {index,1}, {names+4,1}, {index+4,999}}}) {
      auto bad = good; Put(bad, at, value); Check(!Parse(bad), "invalid field");
    }
    auto bad = good; bad[0] = 'X'; Check(!Parse(bad), "signature");
    bad = good; bad[names+12] = 'X'; Check(!Parse(bad), "missing logo");
    bad = good; bad[index-1] ^= 1; Check(!Parse(bad), "zlib checksum");
    bad = good; bad[24] = 0; Check(!Parse(bad), "zlib header");
    bad = good; Put(bad,index+20,200); Check(!Parse(bad), "inflate size mismatch");
    bad = good; bad.insert(bad.end(), good.begin()+names, good.end()); Put(bad,20,2);
    Check(!Parse(bad), "duplicate name");
    for (auto [at,value] : std::array<std::pair<size_t,uint32_t>, 7>{{
        {4,123},{12,0},{16,0xffffffff},{76,0},{84,0},{28,2},{112,0x200}}}) {
      auto dds = Dds(); Put(dds,at,value); Check(!Parse(Sra(dds)), "invalid DDS");
    }
    Check(!narutorise::menu::game_logo::ReadLogo(good.size(), [](uint64_t,std::span<uint8_t>){return false;}), "read failure");
    const auto folder = output / std::filesystem::path(u8"jogo-ação"); std::filesystem::create_directories(folder);
    Write(folder / "Naruto_clip.SRA", good);
    auto local = narutorise::menu::game_logo::Load(folder, false); Check(bool(local), "folder"); Pixels(*local);
    Write(output / "synthetic.iso", Iso(good));
    auto disc = narutorise::menu::game_logo::Load(output / "synthetic.iso", true); Check(bool(disc), "ISO"); Pixels(*disc);
    auto truncated_iso = Iso(good); truncated_iso.resize(34*2048 + good.size() - 1);
    Write(output / "truncated.iso", truncated_iso);
    Check(!narutorise::menu::game_logo::Load(output / "truncated.iso", true), "truncated ISO extent");
    Check(!narutorise::menu::game_logo::Load(output / "absent", false), "missing folder");
    Check(!narutorise::menu::game_logo::Load(output / "absent.iso", true), "missing ISO");
    for (int i=2; i<argc; ++i) {
      const auto path = std::filesystem::path(argv[i]);
      auto real = narutorise::menu::game_logo::Load(path, !std::filesystem::is_directory(path));
      Check(real && real->width==528 && real->height==288, "real local logo");
      Check(std::any_of(real->rgba.begin(),real->rgba.end(),[](uint8_t b){return b!=0;}), "real pixels");
      Write(output / ("real-"+std::to_string(i)+".rgba"),real->rgba);
      std::cout << "PASS: real logo " << path.string() << " 528x288\n";
    }
    std::cout << "PASS: bounded SRA, zlib integrity, BC3 color/alpha, malformed input, folder and synthetic ISO\n";
  } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
