#pragma once

// Helpers of the journal records of the D3D boundary: JSON values of guest
// floats and of tool text, and hex dumps of guest and physical memory.
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#include <rex/system/thread_state.h>
#include "gpu/common/guest_memory.h"
#include "gpu/host/services.h"

namespace gpu {
// JSON value of a float read from guest data; non-finite values are null.
inline std::string JsonFloat(float f) { return std::isfinite(f) ? fmt::format("{}", f) : "null"; }
// Contents of a JSON string from tool text (DXC messages).
inline std::string JsonText(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (uint8_t(c) < 0x20) out += ' ';
    else out += c;
  }
  return out;
}
inline std::string GuestHex(uint8_t* base, uint32_t address, uint32_t size) {
  std::vector<uint8_t> bytes(size);
  auto* memory = GuestMemory();  // A record built on the translator thread has no guest context.
  if (!address || !memory || !ReadVirtual(memory, address, bytes.data(), size)) return "";
  std::string out;
  out.reserve(size * 2);
  for (uint8_t b : bytes) out += fmt::format("{:02x}", b);
  return out;
}
inline std::string PhysicalHex(uint8_t* base, uint32_t physical, uint32_t size) {
  if (physical >= 0x20000000u || size > 0x10000u || size > 0x20000000u - physical) return "";
  std::vector<uint8_t> bytes(size);
  if (!ReadPhysical(base, physical, size, bytes.data())) return "";
  std::string out;
  for (uint8_t b : bytes) out += fmt::format("{:02x}", b);
  return out;
}
}  // namespace gpu
