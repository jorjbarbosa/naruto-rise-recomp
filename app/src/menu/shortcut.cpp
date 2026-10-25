// Windows shell integration; tests write links under their own output directory.
#include "menu/shortcut.h"
#include "launch_arguments.h"
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#endif
namespace narutorise::menu {
std::filesystem::path DesktopShortcut() {
#ifdef _WIN32
  PWSTR path = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path))) return {};
  auto result = std::filesystem::path(path) / "Naruto - Rise of a Ninja.lnk";
  CoTaskMemFree(path); return result;
#else
  return {};
#endif
}
bool WriteShortcut(const std::filesystem::path& link_path, const std::filesystem::path& executable,
                    const std::filesystem::path& user_root) {
#ifdef _WIN32
  if (link_path.empty()) return false;
  const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  struct Scope { bool uninit; ~Scope() { if (uninit) CoUninitialize(); } } scope{SUCCEEDED(initialized)};
  Microsoft::WRL::ComPtr<IShellLinkW> link;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
  const auto arguments = launch::UserDataArgument(user_root);
  if (FAILED(link->SetPath(executable.c_str())) || FAILED(link->SetWorkingDirectory(executable.parent_path().c_str())) ||
      FAILED(link->SetIconLocation(executable.c_str(), 0)) || FAILED(link->SetArguments(arguments.c_str()))) return false;
  Microsoft::WRL::ComPtr<IPersistFile> file;
  return SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(link_path.c_str(), TRUE));
#else
  return false;
#endif
}
}  // namespace narutorise::menu
