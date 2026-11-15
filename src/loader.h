#pragma once

#include <filesystem>

// Loads default.xex into guest memory, resolves the kernel data imports and fills the indirect call
// table. Fills kernel::g_module.
bool LoadExecutable(const std::filesystem::path& path);
