// A driver that ignores the active callback models XInput and replay devices.
#include "menu/input_gate.h"
#include "menu/shortcut.h"
#include "gpu/common/shader_activity.h"
#include <filesystem>
#include <iostream>
#include <cmath>
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;
class Driver final : public InputDriver {
 public:
  Driver() : InputDriver(nullptr, 0) {}
  uint16_t buttons = X_INPUT_GAMEPAD_A;
  int strokes = 5;
  bool close_during_read = false;
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>&) override {}
  X_RESULT GetDeviceState(DeviceId, X_INPUT_STATE* out) override {
    *out = {}; out->packet_number = 99; out->gamepad.buttons = buttons;
    if (close_during_read) { close_during_read = false; narutorise::menu::input_blocked = false; out->gamepad = {}; }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(DeviceId, uint32_t, X_INPUT_CAPABILITIES*) override { return X_ERROR_SUCCESS; }
  X_RESULT SetDeviceVibration(DeviceId, X_INPUT_VIBRATION*) override { return X_ERROR_SUCCESS; }
  X_RESULT GetDeviceKeystroke(DeviceId, uint32_t, X_INPUT_KEYSTROKE* out) override {
    *out = {}; if (strokes-- > 0) { out->virtual_key = 0x5800; return X_ERROR_SUCCESS; } return X_ERROR_EMPTY;
  }
};
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  using namespace narutorise::menu;
  input_gate_enabled = true;
  auto driver = std::make_unique<Driver>(); auto* raw = driver.get();
  auto gated = GateInput(std::move(driver));
  X_INPUT_STATE state; const auto id = static_cast<DeviceId>(1);
  gated->GetDeviceState(id, &state);
  if (uint16_t(state.gamepad.buttons) != X_INPUT_GAMEPAD_A) return 3;
  const uint32_t before = state.packet_number;
  input_blocked = true; gated->GetDeviceState(id, &state);
  if (uint16_t(state.gamepad.buttons) || uint32_t(state.packet_number) == before) return 4;
  if (raw->strokes > 0) return 14; // State polling alone must discard queued menu presses.
  X_INPUT_KEYSTROKE stroke;
  if (gated->GetDeviceKeystroke(id, 0, &stroke) != X_ERROR_EMPTY || raw->strokes > 0) return 5;
  raw->close_during_read = true; gated->GetDeviceState(id, &state);
  gated->GetDeviceState(id, &state); // A neutral SDK read at close is not a physical release.
  if (uint16_t(state.gamepad.buttons)) return 6;
  raw->buttons = 0; gated->GetDeviceState(id, &state);
  raw->buttons = X_INPUT_GAMEPAD_Y; gated->GetDeviceState(id, &state);
  if (uint16_t(state.gamepad.buttons) != X_INPUT_GAMEPAD_Y) return 7;
  const auto jobs = gpu::shader_jobs.load();
  { gpu::ShaderCompilation activity;
    if (!gpu::ShaderCompilationVisible() || gpu::shader_jobs.load() != jobs + 1) return 12; }
  if (gpu::shader_jobs.load() != jobs || !gpu::ShaderCompilationVisible()) return 13;
  gpu::BeginShaderBatch(10);
  { gpu::ShaderBatchEntry first, second; }
  const auto progress = gpu::CurrentShaderProgress();
  if (progress.pending != 8 || std::abs(progress.fraction - .2f) > .001f) return 15;
  gpu::EndShaderBatch();
  const auto folder = std::filesystem::absolute(argv[1]); std::filesystem::create_directories(folder);
  const auto executable = folder / "narutorise.exe", link_path = folder / "game.lnk", user = folder / "user";
  if (!WriteShortcut(link_path, executable, user)) return 8;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Microsoft::WRL::ComPtr<IShellLinkW> link;
  Microsoft::WRL::ComPtr<IPersistFile> file;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
      FAILED(link.As(&file)) || FAILED(file->Load(link_path.c_str(), STGM_READ))) return 9;
  wchar_t target[MAX_PATH], arguments[2048], cwd[MAX_PATH];
  if (FAILED(link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH)) ||
      FAILED(link->GetArguments(arguments, 2048)) || FAILED(link->GetWorkingDirectory(cwd, MAX_PATH))) return 10;
  if (std::filesystem::path(target) != executable || std::filesystem::path(cwd) != folder ||
      std::wstring(arguments).find(user.wstring()) == std::wstring::npos ||
      std::wstring(arguments).find(L"game_data_root") != std::wstring::npos) return 11;
  const std::filesystem::path roots[] = {folder / L"saves-a\u00e7\u00e3o", L"D:\\saves\\", L"D:\\", L"..\\relative saves"};
  for (const auto& root : roots) {
    const auto expected = std::filesystem::absolute(root).lexically_normal();
    file.Reset(); link.Reset();
    if (!WriteShortcut(link_path, executable, root) ||
        FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&file)) || FAILED(file->Load(link_path.c_str(), STGM_READ)) ||
        FAILED(link->GetArguments(arguments, 2048))) return 16;
    int count = 0;
    auto parsed = CommandLineToArgvW((L"narutorise.exe " + std::wstring(arguments)).c_str(), &count);
    const std::wstring prefix = L"--user_data_root=";
    const bool equal = parsed && count == 2 && std::wstring(parsed[1]).starts_with(prefix) &&
        std::filesystem::path(std::wstring(parsed[1]).substr(prefix.size())) == expected;
    if (parsed) LocalFree(parsed);
    if (!equal) return 17;
  }
  file.Reset(); link.Reset(); CoUninitialize();
  std::cout << "PASS: neutral state/packet, drained keystrokes, release latch, restored input, direct game shortcut\n";
}
