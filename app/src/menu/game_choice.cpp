// The Windows picker belongs to the game window; only the XEX is hashed.
#include "menu/game_choice.h"
#include "menu/text.h"
#include <fstream>
#include <toml++/toml.hpp>
#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#endif

namespace narutorise::menu {
FileChoice ChooseFile(void* owner, bool folder) {
#ifdef _WIN32
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  struct ComScope { bool uninit; ~ComScope() { if (uninit) CoUninitialize(); } } scope{SUCCEEDED(initialized)};
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return {{}, "The file picker could not initialize"};
  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    return {{}, "The file picker is unavailable"};
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                      (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
  if (!folder) {
    const auto wide = [](const char* text) {
      const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
      std::wstring value(size_t(length), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, text, -1, value.data(), length);
      return value;
    };
    const auto game = wide(Text("game-filter")), all = wide(Text("all-files"));
    const COMDLG_FILTERSPEC filters[] = {{game.c_str(), L"*.iso;default.xex"}, {all.c_str(), L"*.*"}};
    dialog->SetFileTypes(2, filters);
  }
  const HRESULT shown = dialog->Show(static_cast<HWND>(owner));
  if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
  if (FAILED(shown)) return {{}, "The file picker could not open"};
  Microsoft::WRL::ComPtr<IShellItem> item;
  PWSTR name = nullptr;
  if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)))
    return {{}, "The selected path could not be read"};
  std::filesystem::path path(name);
  CoTaskMemFree(name);
  return {std::move(path), {}};
#else
  return {{}, "The system file picker is unavailable on this platform"};
#endif
}

bool SaveGameRoot(const std::filesystem::path& config, const std::filesystem::path& root, std::string& error) {
  auto temporary = config;
  temporary += ".tmp";
  try {
    toml::table table;
    if (std::filesystem::exists(config)) {
      std::ifstream input(config, std::ios::binary);
      table = toml::parse(input);
    }
    const auto utf8 = root.u8string();
    table.insert_or_assign("custom_game_root", std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << table;
    output.close();
    if (!output) throw std::runtime_error("Could not write the configuration");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), config.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Could not replace the configuration");
#else
    std::filesystem::rename(temporary, config);
#endif
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
  }
}
bool GameChoice::Start(const std::filesystem::path& path) {
  if (Busy()) return false;
  started_ = std::chrono::steady_clock::now();
  result_ = std::async(std::launch::async, [path] {
    Result result{path, game_image::Identify(path)};
    if (result.game.status == game_image::Status::kReady) {
      std::error_code ec;
      if (!result.game.disc_image && !std::filesystem::is_directory(path, ec)) result.root = path.parent_path();
      result.root = std::filesystem::weakly_canonical(result.root);
    }
    return result;
  });
  return true;
}
bool GameChoice::Slow() const {
  return Busy() && std::chrono::steady_clock::now() - started_ >= std::chrono::milliseconds(250);
}
std::optional<GameChoice::Result> GameChoice::Poll() {
  if (!Busy() || result_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return {};
  try { return result_.get(); } catch (...) { return Result{}; }
}
}  // namespace narutorise::menu
