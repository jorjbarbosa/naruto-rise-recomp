// Loading of the recompiled guest modules: default.xex at startup and AI2C.dll on XexLoadImage.

#include "loader.h"

#include "ascii.h"
#include "kernel/kernel.h"
#include "ppc/ppc_module.h"
#include "vfs/vfs.h"

#include <image.h>
#include <xex.h>

#include <atomic>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

// Ordinal -> name tables of XenonUtils (xex.cpp).
extern std::unordered_map<size_t, const char*> XboxKernelExports;
extern std::unordered_map<size_t, const char*> XamExports;

namespace
{
    // Kernel data exports have no thunk: their import slot must hold the address of the data.
    // XenonUtils leaves the slot byte-swapped, so it reads natively as the original record.
    void ResolveVariableImports(const std::vector<uint8_t>& file)
    {
        auto* header = static_cast<const Xex2ImportHeader*>(getOptHeaderPtr(file.data(), XEX_HEADER_IMPORT_LIBRARIES));
        if (header == nullptr)
            return;

        std::vector<std::string_view> names;
        auto* strings = reinterpret_cast<const char*>(header + 1);
        size_t offset = 0;
        for (uint32_t i = 0; i < header->numImports; i++)
        {
            names.emplace_back(strings + offset);
            offset += ((names.back().size() + 1) + 3) & ~size_t(3);
        }

        auto* library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const uint8_t*>(header) +
            sizeof(Xex2ImportHeader) + header->sizeOfStringTable);
        for (uint32_t i = 0; i < header->numImports; i++)
        {
            std::string module(names[library->name & 0xFF]);
            auto& table = module == "xam.xex" ? XamExports : XboxKernelExports;
            auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);

            for (uint32_t j = 0; j < library->numberOfImports; j++)
            {
                uint32_t slot = descriptors[j].firstThunk;
                uint32_t record;
                memcpy(&record, mem::Ptr(slot), 4);
                if ((record >> 24) != 0) // function thunk
                    continue;

                auto name = table.find(record & 0xFFFF);
                if (name == table.end())
                    continue;
                // The table holds "__imp__Name".
                const char* plain = name->second + 7;
                uint32_t address = kernel::ResolveVariable(module.c_str(), plain);
                if (address != 0)
                {
                    mem::Store32(slot, address);
                    TRACE("import: %s!%s -> %08X", module.c_str(), plain, address);
                }
            }

            library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const uint8_t*>(library) + library->size);
        }
    }

    // Guest copy of the XEX header (RtlImageXexHeaderField, XamGetExecutionId), returned in xexHeader,
    // and a minimal loader entry: image base, entry, size and the XEX header at +0x58. The entry
    // address is the module handle.
    uint32_t CreateModuleHandle(const std::vector<uint8_t>& file, const Image& image, uint32_t& xexHeader)
    {
        uint32_t headerSize = reinterpret_cast<const Xex2Header*>(file.data())->headerSize;
        xexHeader = mem::Alloc(mem::Heap::System, headerSize, 0x1000);
        memcpy(mem::Ptr(xexHeader), file.data(), headerSize);

        uint32_t handle = mem::Alloc(mem::Heap::System, 0x100);
        mem::Store32(handle + 0x18, uint32_t(image.base));
        mem::Store32(handle + 0x1C, uint32_t(image.entry_point));
        mem::Store32(handle + 0x20, image.size);
        mem::Store32(handle + 0x58, xexHeader);
        return handle;
    }

    // Writes the module's functions into its indirect call table (present) or removes them. Returns
    // the number of functions.
    size_t SetIndirectCallTable(const PPCModuleInfo& module, bool present)
    {
        size_t functions = 0;
        for (auto* mapping = module.mappings; mapping->host != nullptr; mapping++, functions++)
            *PPCModuleSlot(module, mem::g_base, uint32_t(mapping->guest)) = present ? mapping->host : nullptr;
        return functions;
    }

    // The image of a recompiled module from its file (named name in the log; read is false when it
    // could not be read), checked against the code built for it. Returns X_STATUS_SUCCESS,
    // X_STATUS_NO_SUCH_FILE when the file could not be read, or X_STATUS_INVALID_IMAGE_FORMAT.
    uint32_t ParseModuleImage(const char* name, bool read, const std::vector<uint8_t>& file,
        const PPCModuleInfo& module, Image& image)
    {
        if (!read || file.empty())
        {
            Log("error: cannot read %s", name);
            return kernel::X_STATUS_NO_SUCH_FILE;
        }
        image = Image::ParseImage(file.data(), file.size());
        if (image.data == nullptr)
        {
            Log("error: %s is not a supported XEX", name);
            return kernel::X_STATUS_INVALID_IMAGE_FORMAT;
        }
        if (image.base != module.imageBase || image.size != module.imageSize)
        {
            Log("error: %s does not match the recompiled code (base %08zX, size %08X)", name, image.base,
                image.size);
            return kernel::X_STATUS_INVALID_IMAGE_FORMAT;
        }
        return kernel::X_STATUS_SUCCESS;
    }

    // A recompiled DLL. Entries live for the whole run, so a module unloaded and loaded again keeps
    // its handle. All fields but loaded are guarded by g_modulesMutex; loaded is read without it by
    // FindGuestFunction, on every call across modules.
    struct GuestModule
    {
        const PPCModuleInfo* info = nullptr;
        uint32_t handle = 0;
        uint32_t entryPoint = 0;
        uint32_t exportTable = 0;
        int32_t references = 0;
        std::atomic<bool> loaded = false; // the indirect call table holds its functions
    };

    constexpr uint32_t kDllProcessDetach = 0;
    constexpr uint32_t kDllProcessAttach = 1;

    // Recursive: the entry point of a module runs under it and may load modules itself.
    std::recursive_mutex g_modulesMutex;
    GuestModule g_modules[] = { { &g_ppcModule_ai2c } };

    GuestModule* ModuleByName(std::string_view fileName)
    {
        for (auto& module : g_modules)
        {
            if (ascii::EqualNoCase(fileName, module.info->fileName))
                return &module;
        }
        return nullptr;
    }

    GuestModule* LoadedModuleByHandle(uint32_t handle)
    {
        for (auto& module : g_modules)
        {
            if (module.references > 0 && module.handle == handle)
                return &module;
        }
        return nullptr;
    }

    // Runs _DllMainCRTStartup(handle, reason, 0) of the module. The import that got here is a call
    // for the guest, so the entry point may change the volatile registers, and the ABI makes it keep
    // r1, r2, r13 and the non-volatile ones; only LR is put back.
    bool RunEntryPoint(const GuestModule& module, uint32_t reason, PPCContext& ctx, uint8_t* base)
    {
        PPCFunc* entry = *PPCModuleSlot(*module.info, base, module.entryPoint);
        if (entry == nullptr)
        {
            Log("%s: entry point %08X is not a recompiled function", module.info->fileName, module.entryPoint);
            return false;
        }
        uint64_t lr = ctx.lr;
        ctx.r3.u64 = module.handle;
        ctx.r4.u64 = reason;
        ctx.r5.u64 = 0;
        entry(ctx, base);
        ctx.lr = lr;
        return ctx.r3.u32 != 0;
    }
}

bool LoadExecutable(const vfs::Source& game)
{
    std::vector<uint8_t> file;
    bool read = vfs::ReadWholeFile(game, "default.xex", file);
    Image image;
    if (ParseModuleImage("default.xex", read, file, g_ppcModule_default, image) != kernel::X_STATUS_SUCCESS)
        return false;

    memcpy(mem::Ptr(uint32_t(image.base)), image.data.get(), image.size);

    auto& module = kernel::g_module;
    module.imageBase = uint32_t(image.base);
    module.imageSize = image.size;
    module.entryPoint = uint32_t(image.entry_point);
    module.moduleHandle = CreateModuleHandle(file, image, module.xexHeader);

    auto* stackSize = static_cast<const be<uint32_t>*>(getOptHeaderPtr(file.data(), XEX_HEADER_DEFAULT_STACK_SIZE));
    module.stackSize = stackSize != nullptr ? uint32_t(*stackSize) : 256 * 1024;

    ResolveVariableImports(file);
    size_t functions = SetIndirectCallTable(g_ppcModule_default, true);

    Log("loaded default.xex: image %08X-%08X, entry %08X, %zu functions, stack %u KB", module.imageBase,
        module.imageBase + module.imageSize, module.entryPoint, functions, module.stackSize / 1024);
    return true;
}

uint32_t LoadGuestModule(const std::string& guestPath, PPCContext& ctx, uint8_t* base, uint32_t& status)
{
    std::lock_guard lock(g_modulesMutex);
    std::string_view fileName = guestPath;
    fileName.remove_prefix(fileName.find_last_of("\\/") + 1);
    GuestModule* module = ModuleByName(fileName);
    if (module == nullptr)
    {
        Log("XexLoadImage(\"%s\"): no recompiled module with that name", guestPath.c_str());
        status = kernel::X_STATUS_NO_SUCH_FILE;
        return 0;
    }
    if (module->references > 0)
    {
        module->references++;
        status = kernel::X_STATUS_SUCCESS;
        return module->handle;
    }

    // Loaded (again): a fresh image, so its data starts as the file has it.
    std::vector<uint8_t> file;
    bool read = kernel::ReadGuestFile(guestPath, file);
    Image image;
    status = ParseModuleImage(guestPath.c_str(), read, file, *module->info, image);
    if (status != kernel::X_STATUS_SUCCESS)
        return 0;
    memcpy(mem::Ptr(uint32_t(image.base)), image.data.get(), image.size);
    ResolveVariableImports(file);
    if (module->handle == 0)
    {
        uint32_t xexHeader;
        module->handle = CreateModuleHandle(file, image, xexHeader);
    }
    module->entryPoint = uint32_t(image.entry_point);
    auto* header = reinterpret_cast<const Xex2Header*>(file.data());
    module->exportTable = reinterpret_cast<const Xex2SecurityInfo*>(file.data() + header->securityOffset)->exportTable;
    size_t functions = SetIndirectCallTable(*module->info, true);
    module->loaded = true;

    module->references = 1;
    if (!RunEntryPoint(*module, kDllProcessAttach, ctx, base))
    {
        Log("XexLoadImage(\"%s\"): the entry point failed", guestPath.c_str());
        module->references = 0;
        module->loaded = false;
        SetIndirectCallTable(*module->info, false);
        status = kernel::X_STATUS_DLL_INIT_FAILED;
        return 0;
    }

    Log("loaded %s: image %08X-%08X, entry %08X, %zu functions, handle %08X", module->info->fileName,
        uint32_t(image.base), uint32_t(image.base + image.size), module->entryPoint, functions, module->handle);
    status = kernel::X_STATUS_SUCCESS;
    return module->handle;
}

bool UnloadGuestModule(uint32_t handle, PPCContext& ctx, uint8_t* base)
{
    std::lock_guard lock(g_modulesMutex);
    GuestModule* module = LoadedModuleByHandle(handle);
    if (module == nullptr)
        return false;
    if (--module->references > 0)
        return true;

    RunEntryPoint(*module, kDllProcessDetach, ctx, base);
    module->loaded = false;
    SetIndirectCallTable(*module->info, false);
    Log("unloaded %s", module->info->fileName);
    return true;
}

PPCFunc* FindGuestFunction(uint32_t address)
{
    if (PPCModuleContains(g_ppcModule_default, address))
        return *PPCModuleSlot(g_ppcModule_default, mem::g_base, address);
    for (auto& module : g_modules)
    {
        if (module.loaded && PPCModuleContains(*module.info, address))
            return *PPCModuleSlot(*module.info, mem::g_base, address);
    }
    return nullptr;
}

void CallForeignFunction(uint32_t target, PPCContext& ctx, uint8_t* base)
{
    PPCFunc* function = FindGuestFunction(target);
    if (function == nullptr)
        ReportBadIndirectCall(target, ctx);
    function(ctx, base);
}

uint32_t FindModuleExport(uint32_t handle, uint32_t ordinal)
{
    std::lock_guard lock(g_modulesMutex);
    GuestModule* module = LoadedModuleByHandle(handle);
    if (module == nullptr || module->exportTable == 0)
        return 0;

    // XEX export table: magic (3 words), module number (2), version (3), image base >> 16, count,
    // first ordinal, then the image offset of each ordinal.
    uint32_t table = module->exportTable;
    if (mem::Load32(table + 4) != 0x00485645)
        return 0;
    uint32_t imageBase = mem::Load32(table + 0x20) << 16;
    uint32_t count = mem::Load32(table + 0x24);
    uint32_t first = mem::Load32(table + 0x28);
    if (ordinal - first >= count)
        return 0;
    uint32_t offset = mem::Load32(table + 0x2C + (ordinal - first) * 4);
    return offset != 0 ? imageBase + offset : 0;
}
