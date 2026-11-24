#pragma once

#include <ppc_context.h>

#include <cstdint>
#include <string>

namespace vfs
{
    class Source;
}

// Loads default.xex of the game's files into guest memory, resolves the kernel data imports and fills
// the indirect call table. Fills kernel::g_module. False when it cannot be read or is not the
// recompiled executable (logged).
bool LoadExecutable(const vfs::Source& game);

// ---- Recompiled DLLs (XexLoadImage), thread-safe ----
//
// Only modules recompiled into the runtime can be loaded (AI2C.dll); the file name of the guest
// path picks the module. The entry point runs on the calling guest thread, inside the import, so
// these take the import's context.

// Loads the module (image, data imports, indirect call table, entry point with DLL_PROCESS_ATTACH),
// or adds a reference when it is already loaded. Returns the module handle, or 0 with the failure in
// status.
uint32_t LoadGuestModule(const std::string& guestPath, PPCContext& ctx, uint8_t* base, uint32_t& status);

// Drops a reference; the last one runs the entry point with DLL_PROCESS_DETACH and removes the
// module's functions from the indirect call table. False when handle is not a loaded module.
bool UnloadGuestModule(uint32_t handle, PPCContext& ctx, uint8_t* base);

// Address of an export of a loaded module by ordinal, from its XEX export table, or 0.
uint32_t FindModuleExport(uint32_t handle, uint32_t ordinal);

// Recompiled function at a guest address in default.xex or a loaded DLL, or nullptr. Lock-free: it
// runs on every indirect call across modules.
PPCFunc* FindGuestFunction(uint32_t address);
