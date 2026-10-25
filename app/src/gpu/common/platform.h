#pragma once

// Host operating system services of the native renderer that the SDK does not
// give the way the renderer uses them: the monotonic tick clock and the fine
// wait of the Swap limiter, the scheduling of the worker pools, the CPU times
// of a thread, the debug keys, the error box of a stop and the immediate exit.
// One implementation per system, picked by CMake: platform_win32.cpp keeps the
// calls the renderer always made on Windows, platform_posix.cpp is the rest.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gpu::platform {
// The monotonic host clock in ticks, TickFrequency() of them per second (the
// QPC on Windows, CLOCK_MONOTONIC in nanoseconds elsewhere).
int64_t Ticks();
int64_t TickFrequency();
// Sleeps about `ticks` of that clock with the finest timer of the system (a
// high-resolution waitable timer of the calling thread on Windows,
// clock_nanosleep elsewhere). False when the timer could not be armed.
bool Sleep(int64_t ticks);
// One step of a spin wait.
void Pause();

// The scheduling of the calling thread, for the worker pools. Outside Windows
// an unprivileged thread cannot rise above normal: kLowest is the batch policy,
// kNormal and kHighest the normal one.
enum class ThreadPriority { kLowest, kNormal, kHighest };
void SetThreadPriority(ThreadPriority priority);
// The CPU times of a host thread by its native handle (rex::thread::Thread),
// in 100 ns units, by the native_handle() of a running rex::thread::Thread
// (outside Windows the handle of an ended thread may be gone). Outside Windows
// the system gives the total only, reported as user time.
bool ThreadTimes(void* native_thread, uint64_t& kernel_100ns, uint64_t& user_100ns);

// A window to present to, by the handles of the system: what a Vulkan
// surface or a DXGI swap chain is made from. `handle` is the SDK's native
// handle of the window (rex::ui::Window::GetNativeWindowHandle: the HWND on
// Windows, the SDL_Window elsewhere), which ClientSize asks.
struct NativeWindow {
  enum class Kind { kNone, kWin32, kXlib, kWayland };
  Kind kind = Kind::kNone;
  void* display = nullptr;  // HINSTANCE, X11 Display*, wl_display*
  void* window = nullptr;   // HWND, X11 Window (the number), wl_surface*
  void* handle = nullptr;
};
NativeWindow WindowHandles(void* native_handle);
// The window's client area in pixels; false while it is gone or minimized.
bool ClientSize(const NativeWindow& window, uint32_t& width, uint32_t& height);
// The window's state as JSON fields for the records (client size, and on
// Windows its DPI, monitor and refresh rate).
std::string WindowFacts(const NativeWindow& window);
// The close of the window's close box (WM_CLOSE on Windows, SDL's close
// request elsewhere): the SDK asks its listeners before it closes.
bool PostWindowClose(const NativeWindow& window);

// The debug keys of the diagnostics and of the interpolation, read at any
// moment (without the window's events on Windows; from SDL's keyboard state
// elsewhere, so the game window has to have focus).
enum class Key { kF6, kF7, kF8, kF9, kF10 };
bool KeyDown(Key key);

// Whether the host page of `address` is committed and readable without a
// fault (a guard page is not), with the extent of the region that shares its
// state, so a walk over nearby addresses asks again only past `end`. For the
// crash report, which reads where a fault may have left anything.
bool ReadableRegion(const void* address, uintptr_t& begin, uintptr_t& end);

// The path of the running executable, and of every module the process has
// loaded (UTF-8), for the journal; empty when the system does not give them.
std::filesystem::path ExecutablePath();
std::vector<std::string> LoadedModules();

// A blocking error box above the other windows, for the stop of the renderer.
void ShowError(std::string_view title, std::string_view text);
// Ends the process at once with the code, without exit handlers.
[[noreturn]] void Terminate(uint32_t code);
}  // namespace gpu::platform
