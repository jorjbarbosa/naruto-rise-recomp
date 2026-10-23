// The Windows side of gpu/common/platform.h: the calls the renderer made
// before the services had a header of their own.
#include "gpu/common/platform.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <windows.h>
#include <tlhelp32.h>
#include <fmt/format.h>
#include <rex/string.h>

namespace gpu::platform {
int64_t Ticks() {
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}
int64_t TickFrequency() {
  static const int64_t frequency = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  return frequency;
}
bool Sleep(int64_t ticks) {
  thread_local HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) return false;
  LARGE_INTEGER due{};  // Relative, in 100 ns units.
  due.QuadPart = -(std::max<int64_t>)(1, ticks * 10000000 / TickFrequency());
  if (!SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) return false;
  WaitForSingleObject(timer, 1000);
  return true;
}
void Pause() { YieldProcessor(); }

void SetThreadPriority(ThreadPriority priority) {
  const int value = priority == ThreadPriority::kLowest    ? THREAD_PRIORITY_LOWEST
                    : priority == ThreadPriority::kHighest ? THREAD_PRIORITY_HIGHEST
                                                           : THREAD_PRIORITY_NORMAL;
  ::SetThreadPriority(GetCurrentThread(), value);
}
bool ThreadTimes(void* native_thread, uint64_t& kernel_100ns, uint64_t& user_100ns) {
  FILETIME created{}, exited{}, kernel_time{}, user_time{};
  if (!native_thread ||
      !GetThreadTimes(static_cast<HANDLE>(native_thread), &created, &exited, &kernel_time, &user_time))
    return false;
  auto ticks = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  kernel_100ns = ticks(kernel_time);
  user_100ns = ticks(user_time);
  return true;
}

NativeWindow WindowHandles(void* native_handle) {
  NativeWindow window;
  if (!native_handle) return window;
  window.kind = NativeWindow::Kind::kWin32;
  window.display = GetModuleHandleW(nullptr);
  window.window = native_handle;
  window.handle = native_handle;
  return window;
}
bool ClientSize(const NativeWindow& window, uint32_t& width, uint32_t& height) {
  const HWND hwnd = static_cast<HWND>(window.window);
  RECT rect{};
  if (window.kind != NativeWindow::Kind::kWin32 || !IsWindow(hwnd) || IsIconic(hwnd) || !GetClientRect(hwnd, &rect) ||
      rect.right <= 0 || rect.bottom <= 0)
    return false;
  width = uint32_t(rect.right);
  height = uint32_t(rect.bottom);
  return true;
}

std::string WindowFacts(const NativeWindow& window) {
  const HWND hwnd = static_cast<HWND>(window.window);
  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  const bool have = monitor && GetMonitorInfoW(monitor, &info);
  const bool settings = have && EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode);
  RECT client{};
  const bool sized = hwnd && GetClientRect(hwnd, &client);
  const auto awareness = GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd));
  return fmt::format("\"client\":[{},{}],\"visible\":{},\"dpi\":{},\"dpi_awareness\":{},\"monitor\":[{},{},{},{}],"
                     "\"refresh_hz\":{}", sized ? client.right - client.left : 0, sized ? client.bottom - client.top : 0,
                     IsWindowVisible(hwnd) != 0, GetDpiForWindow(hwnd), int(awareness),
                     have ? info.rcMonitor.left : 0, have ? info.rcMonitor.top : 0,
                     have ? info.rcMonitor.right - info.rcMonitor.left : 0,
                     have ? info.rcMonitor.bottom - info.rcMonitor.top : 0, settings ? mode.dmDisplayFrequency : 0);
}
bool PostWindowClose(const NativeWindow& window) {
  return window.kind == NativeWindow::Kind::kWin32 && PostMessageW(static_cast<HWND>(window.window), WM_CLOSE, 0, 0);
}

bool KeyDown(Key key) {
  const int code = key == Key::kF6   ? VK_F6
                   : key == Key::kF7 ? VK_F7
                   : key == Key::kF8 ? VK_F8
                   : key == Key::kF9 ? VK_F9
                                     : VK_F10;
  return (GetAsyncKeyState(code) & 0x8000) != 0;
}

bool ReadableRegion(const void* address, uintptr_t& begin, uintptr_t& end) {
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT) return false;
  if (!info.Protect || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
  end = begin + info.RegionSize;
  return true;
}

std::filesystem::path ExecutablePath() {
  wchar_t exe[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return length && length < MAX_PATH ? std::filesystem::path(std::wstring(exe, length)) : std::filesystem::path();
}
std::vector<std::string> LoadedModules() {
  std::vector<std::string> modules;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE) return modules;
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (bool more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry))
    modules.push_back(rex::string::to_utf8(std::u16string(reinterpret_cast<const char16_t*>(entry.szExePath))));
  CloseHandle(snapshot);
  return modules;
}

void ShowError(std::string_view title, std::string_view text) {
  const auto wide_title = rex::string::to_utf16(title);
  const auto wide_text = rex::string::to_utf16(text);
  MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(wide_text.c_str()),
              reinterpret_cast<const wchar_t*>(wide_title.c_str()),
              MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
}
void Terminate(uint32_t code) {
  TerminateProcess(GetCurrentProcess(), code);
  std::_Exit(int(code));
}
}  // namespace gpu::platform
