#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Windows integration for the installer wizard: shell shortcuts, the
// per-user Add/Remove Programs entry and delayed self-removal (the running
// .exe cannot be deleted while the wizard is open).
namespace installer_windows {

void CoInitializeForUi();
void CoUninitializeForUi();

std::filesystem::path DesktopShortcutPath();
std::filesystem::path StartMenuGroupDir();
std::filesystem::path StartMenuShortcutPath();

bool CreateShortcut(const std::filesystem::path& target,
                    const std::filesystem::path& working_dir,
                    const std::filesystem::path& shortcut_path,
                    const std::string& description);

bool RegisterUninstallEntry(const std::filesystem::path& install_dir,
                             const std::string& version,
                             const std::filesystem::path& uninstaller_path);
bool RemoveUninstallEntry();

// Schedules an async `cmd.exe` that removes `target` a couple of seconds
// after the wizard exits (rmdir /s /q for folders, del /f /q for files).
bool ScheduleDelayedRemove(const std::filesystem::path& target, bool recursive);

}  // namespace installer_windows
