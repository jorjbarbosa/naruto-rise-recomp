#pragma once

// Textures of the D3D boundary (textures.cpp): the sampled textures of a draw,
// translated once per (physical base, layout) and uploaded again only after an
// event (a write to their pages, the last Unlock, the destruction, a resolve).
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <rex/ppc/context.h>
#include "gpu/backend/engine.h"
#include "gpu/shaders/shaders.h"
#include "gpu/textures/formats.h"

namespace gpu {
// Record of the textures of a draw: samplers of both shaders, the fetch
// constants of the translation's copy and the registered headers with the same base.
std::string TexturesRecord(uint8_t* base, const BoundShader& vs, const BoundShader& ps);
std::array<uint32_t, 6> PhysicalFetch(std::array<uint32_t, 6> d);
bool TextureVerify();
// The fetch constants of the 32 slots (6 words each at device +0x480 + 24 k),
// as the last draw commit left them. The title thread reads the dirty
// slots before the original commit of each draw and sends the ones that
// changed (CaptureFetch, against its copy of what was sent); the translation
// applies them to its own copy, the one BindTexture, TextureRecord and the
// vertex streams read (ApplyFetch, FetchConstantSlot).
struct FetchDelta {
  bool refresh = false, full = false;  // refresh: the mask or a full refresh asked for a read.
  uint32_t changed = 0;                // Bit 31 - k: slot k changed; its words below.
  std::array<std::array<uint32_t, 6>, 32> dwords;  // Only the changed slots are written (not zeroed: one per draw).
};
void CaptureFetch(uint8_t* base, uint32_t dev, uint32_t mask, bool full, FetchDelta& out);
void ApplyFetch(const FetchDelta& delta);
const std::array<uint32_t, 6>& FetchConstantSlot(uint32_t slot);
// TextureVerify (title thread): the slots whose live words differ from the copy sent.
uint32_t FetchMissedWrites(uint8_t* base, uint32_t dev);
// The draw's texture of one sampler; fetch_missed: the verify found a live
// word the copy missed. False: reason (TextureRecord rebuilds the record).
bool BindTexture(uint8_t* base, const ShaderSampler& sampler, bool vertex, bool fetch_missed, DrawTexture& out,
                 std::string& reason);
// The record of a sampler's texture (the diagnostics and the guards).
std::string TextureRecord(uint8_t* base, uint32_t reg, bool vertex);
// CreateTexture registers the header by its own fetch constant.
void RegisterTextureHeader(uint8_t* base, uint32_t header);
// The last Unlock of a resource (8212DBE0).
void NoteUnlock(PPCContext& ctx, uint8_t* base, uint32_t resource, uint32_t common, uint32_t base_address,
                uint32_t lines, uint32_t mip_address, uint32_t mip_lines);
bool ReleaseTextureBase(uint32_t address, std::string& evidence);
// The bases whose physical memory the title freed without a D750 (Swap, title thread).
bool ReleaseFreedTextureBases(std::string& evidence);
// The copy posted for a base (START evidence).
bool TexturePosted(uint32_t address, uint64_t& layout, uint64_t& version);
std::string TextureRegistrySummary();
// The bases with an engine copy, and the entries registered and the bases
// forgotten since the start.
struct TextureRegistryTotals {
  uint64_t bases = 0, registered = 0, forgotten_bases = 0;
};
TextureRegistryTotals TextureRegistrySnapshot();
extern std::atomic<uint64_t> texture_uploads, texture_binding_reuses;
extern std::atomic<uint64_t> texture_anisotropy_forced;  // Bindings with the forced filter.
}  // namespace gpu
