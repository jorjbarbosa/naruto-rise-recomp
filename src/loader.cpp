#include "loader.h"
#include "kernel/kernel.h"

#include <file.h>
#include <image.h>
#include <xex.h>

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
}

bool LoadExecutable(const std::filesystem::path& path)
{
    std::vector<uint8_t> file = LoadFile(path);
    if (file.empty())
    {
        Log("error: cannot read %s", path.string().c_str());
        return false;
    }

    Image image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr)
    {
        Log("error: %s is not a supported XEX", path.string().c_str());
        return false;
    }
    if (image.base != PPC_IMAGE_BASE || image.size != PPC_IMAGE_SIZE)
    {
        Log("error: %s does not match the recompiled code (base %08zX, size %08X)", path.string().c_str(), image.base, image.size);
        return false;
    }

    memcpy(mem::Ptr(uint32_t(image.base)), image.data.get(), image.size);

    auto& module = kernel::g_module;
    module.imageBase = uint32_t(image.base);
    module.imageSize = image.size;
    module.entryPoint = uint32_t(image.entry_point);

    // Guest copy of the XEX header, for RtlImageXexHeaderField and XamGetExecutionId.
    auto* xexHeader = reinterpret_cast<const Xex2Header*>(file.data());
    uint32_t headerSize = xexHeader->headerSize;
    module.xexHeader = mem::Alloc(mem::Heap::System, headerSize, 0x1000);
    memcpy(mem::Ptr(module.xexHeader), file.data(), headerSize);

    // Minimal loader entry: image base, entry, size and the XEX header at +0x58.
    module.moduleHandle = mem::Alloc(mem::Heap::System, 0x100);
    mem::Store32(module.moduleHandle + 0x18, module.imageBase);
    mem::Store32(module.moduleHandle + 0x1C, module.entryPoint);
    mem::Store32(module.moduleHandle + 0x20, module.imageSize);
    mem::Store32(module.moduleHandle + 0x58, module.xexHeader);

    auto* stackSize = static_cast<const be<uint32_t>*>(getOptHeaderPtr(file.data(), XEX_HEADER_DEFAULT_STACK_SIZE));
    module.stackSize = stackSize != nullptr ? uint32_t(*stackSize) : 256 * 1024;

    ResolveVariableImports(file);

    size_t functions = 0;
    for (auto* mapping = PPCFuncMappings; mapping->host != nullptr; mapping++, functions++)
        PPC_LOOKUP_FUNC(mem::g_base, mapping->guest) = mapping->host;

    Log("loaded %s: image %08X-%08X, entry %08X, %zu functions, stack %u KB", path.filename().string().c_str(),
        module.imageBase, module.imageBase + module.imageSize, module.entryPoint, functions, module.stackSize / 1024);
    return true;
}
