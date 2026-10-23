// Host test of the uploads of the own resources and of their readback, on a
// real device (REX_NATIVE_TEST_API picks the backend). Never enters a guest
// function or the SDK GPU.
// - Every texture format of the contracts, at a size whose levels go down to
//   1 x 1 (blocks narrower than 4 texels in the block formats): each level
//   reads back with the bytes uploaded, its size and its row length.
// - A cube: each level of face 0 reads back as uploaded.
// - Versions: an upload whose version is not above the copy's is skipped and
//   the content stays; a later version replaces it; another layout recreates
//   the copy with its new size.
// - A buffer: created by one range covering it, refreshed by partial ranges
//   at later versions (the bytes outside them kept), a version not above the
//   buffer's skipped, recreated when its size changes.
// - Keys reused while the work recorded with the old resource is in flight: a
//   copy recreated by another layout, a surface destroyed and created again, a
//   buffer grown. The new resource reads back its own content, and the old
//   one is freed once that work completed.
// Usage: native_engine_resource_test <new output.jsonl>
#include "gpu/backend/engine.h"
#include "gpu/backend/tests/test_backend.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace gpu;

namespace {
bool Has(const std::string& evidence, const char* text) { return evidence.find(text) != std::string::npos; }
// The integer after "name": in a counts object; -1 when absent.
long long Count(const std::string& counts, const char* name) {
  const std::string key = std::string("\"") + name + "\":";
  const size_t at = counts.find(key);
  return at == std::string::npos ? -1 : std::strtoll(counts.c_str() + at + key.size(), nullptr, 10);
}
uint8_t Pattern(uint32_t seed, size_t at) { return uint8_t((at * 131 + seed * 29 + (at >> 7) * 17 + 5) & 0xFF); }
std::vector<uint8_t> Bytes(uint32_t seed, size_t count) {
  std::vector<uint8_t> bytes(count);
  for (size_t i = 0; i < count; ++i) bytes[i] = Pattern(seed, i);
  return bytes;
}
// A texture whose subresource i (level i % levels of face i / levels) has the
// bytes of seed + i.
DrawTexture Texture(uint32_t key, TextureFormat format, uint32_t width, uint32_t height, uint32_t levels, bool cube,
                    uint64_t version, uint32_t seed) {
  DrawTexture t;
  t.key = key;
  t.format = format;
  t.width = width;
  t.height = height;
  t.levels = levels;
  t.cube = cube;
  t.layout = (uint64_t(width) << 32) | (uint64_t(height) << 8) | levels;
  t.version = version;
  for (uint32_t i = 0; i < levels * (cube ? 6 : 1); ++i) {
    const LevelRows rows = RowsOfLevel(format, width, height, i % levels);
    t.data.push_back(Bytes(seed + i, size_t(rows.rows) * rows.row_bytes));
  }
  return t;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
  // The debug layer is off by default (backend/d3d12); the host test asks for it.
  if (!std::getenv("REX_NATIVE_D3D12_DEBUG")) SetTestEnvironment("REX_NATIVE_D3D12_DEBUG", "1");
  std::ofstream output(argv[1]);
  if (!output) return 3;
  bool passed = true;
  auto line = [&](const std::string& kind, bool ok, const std::string& evidence) {
    passed = passed && ok;
    output << "{\"kind\":\"" << kind << "\",\"ok\":" << (ok ? "true" : "false") << ",\"evidence\":"
           << (evidence.empty() ? "null" : evidence) << "}\n";
  };
  Engine engine;
  std::string evidence;
  const bool ready = engine.Initialize(evidence, TestRenderMode());
  line("initialize", ready, evidence);
  if (!ready) {
    engine.Shutdown(evidence);
    return 4;
  }
  // Each level of face 0 of the copy at key against the texture's bytes.
  auto check_levels = [&](const std::string& name, const DrawTexture& t) {
    for (uint32_t level = 0; level < t.levels; ++level) {
      std::vector<uint8_t> rows;
      TextureFormat format = TextureFormat::kUnknown;
      uint32_t width = 0, height = 0, row_bytes = 0;
      const bool read = engine.ReadSampled(t.key, level, rows, format, width, height, row_bytes, evidence);
      const LevelRows expected = RowsOfLevel(t.format, t.width, t.height, level);
      const bool ok = read && rows == t.data[level] && format == t.format &&
                      width == std::max(t.width >> level, 1u) && height == std::max(t.height >> level, 1u) &&
                      row_bytes == expected.row_bytes;
      line(name + "-level-" + std::to_string(level), ok, evidence);
    }
  };

  // Every format, levels down to 1 x 1.
  uint32_t key = 0x7000;
  for (TextureFormat format : kTextureFormats) {
    const DrawTexture t = Texture(++key, format, 40, 24, 6, false, 1, uint32_t(format));
    const std::string name = "format-" + std::to_string(uint32_t(format));
    line(name + "-upload", engine.UploadSampled(t, evidence) && Has(evidence, "\"uploaded\":true"), evidence);
    check_levels(name, t);
  }
  // A cube, face 0 of each level.
  const DrawTexture cube = Texture(0x7100, TextureFormat::kR8G8B8A8Unorm, 16, 16, 5, true, 1, 300);
  line("cube-upload", engine.UploadSampled(cube, evidence) && Has(evidence, "\"uploaded\":true"), evidence);
  check_levels("cube", cube);

  // Versions of one copy.
  const DrawTexture first = Texture(0x7200, TextureFormat::kR8G8B8A8Unorm, 32, 16, 1, false, 5, 400);
  line("version-upload", engine.UploadSampled(first, evidence) && Has(evidence, "\"uploaded\":true"), evidence);
  const DrawTexture stale = Texture(0x7200, TextureFormat::kR8G8B8A8Unorm, 32, 16, 1, false, 5, 500);
  line("version-same-skipped", engine.UploadSampled(stale, evidence) && Has(evidence, "\"skipped\":true"), evidence);
  check_levels("version-kept", first);
  const DrawTexture later = Texture(0x7200, TextureFormat::kR8G8B8A8Unorm, 32, 16, 1, false, 6, 600);
  line("version-later", engine.UploadSampled(later, evidence) && Has(evidence, "\"recreated\":false"), evidence);
  check_levels("version-replaced", later);
  const DrawTexture resized = Texture(0x7200, TextureFormat::kR8G8B8A8Unorm, 8, 8, 2, false, 7, 700);
  line("layout-recreated", engine.UploadSampled(resized, evidence) && Has(evidence, "\"recreated\":true"), evidence);
  check_levels("layout-new", resized);

  // A buffer and its partial uploads.
  constexpr uint64_t kBuffer = 0x9000;
  BufferUpload upload;
  upload.key = kBuffer;
  upload.version = 1;
  upload.bytes = 4096;
  upload.ranges.push_back({0, Bytes(800, 4096)});
  std::vector<uint8_t> expected = upload.ranges[0].second;
  std::vector<uint8_t> bytes;
  line("buffer-created", engine.UploadBuffer(upload, evidence) && Has(evidence, "\"created\":true"), evidence);
  line("buffer-read", engine.ReadBufferForTest(kBuffer, bytes, evidence) && bytes == expected, evidence);
  BufferUpload partial;
  partial.key = kBuffer;
  partial.version = 2;
  partial.bytes = 4096;
  partial.ranges.push_back({100, Bytes(900, 50)});
  partial.ranges.push_back({3000, Bytes(901, 1096)});
  for (const auto& [offset, data] : partial.ranges) std::copy(data.begin(), data.end(), expected.begin() + offset);
  line("buffer-partial", engine.UploadBuffer(partial, evidence) && Has(evidence, "\"created\":false"), evidence);
  line("buffer-partial-read", engine.ReadBufferForTest(kBuffer, bytes, evidence) && bytes == expected, evidence);
  partial.ranges = {{0, Bytes(902, 64)}};
  line("buffer-version-skipped", engine.UploadBuffer(partial, evidence) && Has(evidence, "\"skipped\":true"), evidence);
  line("buffer-kept", engine.ReadBufferForTest(kBuffer, bytes, evidence) && bytes == expected, evidence);
  BufferUpload grown;
  grown.key = kBuffer;
  grown.version = 3;
  grown.bytes = 8192;
  grown.ranges.push_back({0, Bytes(1000, 8192)});
  line("buffer-recreated", engine.UploadBuffer(grown, evidence) && Has(evidence, "\"created\":true"), evidence);
  line("buffer-recreated-read", engine.ReadBufferForTest(kBuffer, bytes, evidence) &&
       bytes == grown.ranges[0].second, evidence);

  // Keys reused in flight. A copy recorded into the open batch and recreated
  // in it by another layout: the batch keeps the old image until it completes.
  const DrawTexture before = Texture(0x7300, TextureFormat::kR8G8B8A8Unorm, 16, 16, 1, false, 1, 1100);
  const DrawTexture after = Texture(0x7300, TextureFormat::kR8G8B8A8Unorm, 8, 4, 1, false, 2, 1200);
  line("in-flight-copy-recreated", engine.UploadSampled(before, evidence) && engine.UploadSampled(after, evidence) &&
       Has(evidence, "\"recreated\":true"), evidence);
  check_levels("in-flight-copy-new", after);
  // A surface destroyed while its clear is in flight and created again with its key.
  SurfaceDesc surface;
  surface.key = 0x4500;
  surface.width = surface.height = 16;
  ClearRequest clear;
  clear.color_keys[0] = surface.key;
  clear.clear_color = true;
  clear.color = {1, 0, 0, 1};
  uint64_t fence = 0;
  const bool created = engine.CreateSurface(surface, evidence) && engine.ClearSurfaces(clear, fence, evidence);
  const bool deferred = engine.DestroySurface(surface.key, evidence) && Has(evidence, "\"deferred\":true");
  surface.width = 8;
  clear.color = {0, 0, 1, 1};
  const bool recreated = engine.CreateSurface(surface, evidence) && engine.ClearSurfaces(clear, fence, evidence);
  std::vector<uint8_t> rgba;
  uint32_t width = 0, height = 0;
  const bool read = engine.ReadSurface(surface.key, fence, rgba, width, height, evidence);
  bool blue = read && width == 8 && height == 16 && rgba.size() == size_t(8) * 16 * 4;
  for (size_t i = 0; blue && i < rgba.size(); i += 4)
    blue = rgba[i] == 0 && rgba[i + 1] == 0 && rgba[i + 2] == 255 && rgba[i + 3] == 255;
  line("in-flight-surface-key-reused", created && deferred && recreated && blue, evidence);
  // A buffer grown while its upload is in flight: the old one is retired.
  BufferUpload small;
  small.key = 0x9100;
  small.version = 1;
  small.bytes = 256;
  small.ranges.push_back({0, Bytes(1300, 256)});
  BufferUpload big = small;
  big.version = 2;
  big.bytes = 512;
  big.ranges = {{0, Bytes(1301, 512)}};
  const bool retired_buffer = engine.UploadBuffer(small, evidence) && engine.UploadBuffer(big, evidence) &&
                     Has(evidence, "\"created\":true") && Count(engine.ResourceCounts(), "buffers_retired") == 1;
  line("in-flight-buffer-grown", retired_buffer && engine.ReadBufferForTest(0x9100, bytes, evidence) &&
       bytes == big.ranges[0].second, evidence);
  line("in-flight-freed", engine.WaitIdle(evidence) && Count(engine.ResourceCounts(), "retired_surfaces") == 0 &&
       Count(engine.ResourceCounts(), "retired_buffers") == 0, engine.ResourceCounts());
  line("release-remaining", engine.ReleaseRemaining(evidence), evidence);
  line("shutdown", engine.Shutdown(evidence), evidence);
  output << "{\"kind\":\"verdict\",\"passed\":" << (passed ? "true" : "false") << ",\"guest_executed\":false}\n";
  return passed && output.good() ? 0 : 1;
}
