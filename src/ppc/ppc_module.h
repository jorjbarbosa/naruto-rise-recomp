#pragma once

// A guest module recompiled into the runtime (default.xex, AI2C.dll). CMake generates one
// g_ppcModule_<name> per module (add_ppc_module), compiled against that module's ppc_config.h; the
// runtime, which only sees the ppc_config.h of default.xex, reaches the other modules through it.
// Requires ppc_context.h (PPCFunc, PPCFuncMapping).

#include <cstdint>

struct PPCModuleInfo
{
    const char* fileName; // guest file name, matched case-insensitively
    uint64_t imageBase;
    uint64_t imageSize;
    uint64_t codeBase;
    uint64_t codeSize;
    PPCFuncMapping* mappings; // ends with { 0, nullptr }
};

extern const PPCModuleInfo g_ppcModule_default;
extern const PPCModuleInfo g_ppcModule_ai2c;

// Slot of the module's indirect call table for a code address, as the module's PPC_LOOKUP_FUNC
// computes it: right after the image, 8 bytes per 4-byte instruction.
inline PPCFunc** PPCModuleSlot(const PPCModuleInfo& module, uint8_t* base, uint32_t address)
{
    return reinterpret_cast<PPCFunc**>(base + module.imageBase + module.imageSize + (uint64_t(address - uint32_t(module.codeBase)) * 2));
}

inline bool PPCModuleContains(const PPCModuleInfo& module, uint32_t address)
{
    return address - uint32_t(module.codeBase) < uint32_t(module.codeSize);
}
