#include "installer_windows.h"

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdlib>
#endif

namespace installer_windows {

#if defined(_WIN32)

void CoInitializeForUi() {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  (void)hr;  // RPC_E_CHANGED_MODE etc. are tolerable: shortcuts then use MTA.
}

void CoUninitializeForUi() {
  CoUninitialize();
}

std::filesystem::path GetShellFolder(int csidl) {
  wchar_t buf[MAX_PATH * 4];
  if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, buf))) return {};
  return std::filesystem::path(std::wstring(buf));
}

std::filesystem::path DesktopShortcutPath() {
  return GetShellFolder(CSIDL_DESKTOP) / "Naruto - Rise of a Ninja.lnk";
}

std::filesystem::path StartMenuGroupDir() {
  return GetShellFolder(CSIDL_PROGRAMS) / "Naruto Rise of a Ninja";
}

std::filesystem::path StartMenuShortcutPath() {
  return StartMenuGroupDir() / "Naruto - Rise of a Ninja.lnk";
}

bool CreateShortcut(const std::filesystem::path& target,
                    const std::filesystem::path& working_dir,
                    const std::filesystem::path& shortcut_path,
                    const std::string& description) {
  IShellLinkW* link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&link)))) {
    return false;
  }

  std::wstring target_w = target.wstring();
  std::wstring workdir_w = working_dir.wstring();
  std::wstring desc_w(description.begin(), description.end());

  link->SetPath(target_w.c_str());
  link->SetWorkingDirectory(workdir_w.c_str());
  link->SetDescription(desc_w.c_str());
  link->SetIconLocation(target_w.c_str(), 0);

  IPersistFile* persist = nullptr;
  bool ok = false;
  if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist)))) {
    std::wstring lnk_w = shortcut_path.wstring();
    ok = SUCCEEDED(persist->Save(lnk_w.c_str(), TRUE));
    persist->Release();
  }
  link->Release();
  return ok;
}

constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NarutoRiseRecomp";

bool SetRegString(HKEY key, const wchar_t* name, const std::wstring& value) {
  return RegSetValueExW(key, name, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(value.c_str()),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) ==
         ERROR_SUCCESS;
}

bool RegisterUninstallEntry(const std::filesystem::path& install_dir,
                            const std::string& version,
                            const std::filesystem::path& uninstaller_path) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0,
                      KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }

  std::wstring launcher = (install_dir / "narutorise_launcher.exe").wstring();
  std::wstring uninstall_cmd = L"\"" + uninstaller_path.wstring() + L"\" --uninstall";
  std::wstring version_w(version.begin(), version.end());

  bool ok = SetRegString(key, L"DisplayName", L"Naruto: Rise of a Ninja (PC Port)") &&
            SetRegString(key, L"DisplayVersion", version_w) &&
            SetRegString(key, L"InstallLocation", install_dir.wstring()) &&
            SetRegString(key, L"DisplayIcon", launcher + L",0") &&
            SetRegString(key, L"UninstallString", uninstall_cmd) &&
            SetRegString(key, L"Publisher", L"naruto-rise-recomp") &&
            SetRegString(key, L"URLInfoAbout", L"https://github.com/rexglue");
  if (ok) {
    DWORD one = 1;
    ok = RegSetValueExW(key, L"NoModify", 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&one), sizeof(one)) == ERROR_SUCCESS &&
         RegSetValueExW(key, L"NoRepair", 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&one), sizeof(one)) == ERROR_SUCCESS;
  }

  RegCloseKey(key);
  return ok;
}

bool RemoveUninstallEntry() {
  return RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey) == ERROR_SUCCESS ||
         GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool ScheduleDelayedRemove(const std::filesystem::path& target, bool recursive) {
  wchar_t comspec[1024];
  if (GetEnvironmentVariableW(L"COMSPEC", comspec,
                              ARRAYSIZE(comspec)) == 0) {
    return false;
  }

  std::wstring action = recursive ? L"rmdir /s /q \"" : L"del /f /q \"";
  std::wstring args = L"/c timeout /t 2 /nobreak >nul & " + action +
                      target.wstring() + L"\"";

  std::wstring cmdline = std::wstring(comspec) + L" " + args;
  std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
  buf.push_back(L'\0');

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

#else  // !_WIN32

void CoInitializeForUi() {}
void CoUninitializeForUi() {}
std::filesystem::path DesktopShortcutPath() { return {}; }
std::filesystem::path StartMenuGroupDir() { return {}; }
std::filesystem::path StartMenuShortcutPath() { return {}; }
bool CreateShortcut(const std::filesystem::path&, const std::filesystem::path&,
                    const std::filesystem::path&, const std::string&) {
  return false;
}
bool RegisterUninstallEntry(const std::filesystem::path&, const std::string&,
                            const std::filesystem::path&) {
  return false;
}
bool RemoveUninstallEntry() { return false; }
bool ScheduleDelayedRemove(const std::filesystem::path&, bool) { return false; }

#endif  // _WIN32

}  // namespace installer_windows
