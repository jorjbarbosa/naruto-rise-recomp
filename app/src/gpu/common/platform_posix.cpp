// The POSIX side of gpu/common/platform.h (Linux).
#include "gpu/common/platform.h"

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <string>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <sys/uio.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <rex/memory/utils.h>

namespace gpu::platform {
int64_t Ticks() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return int64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
}
int64_t TickFrequency() { return 1000000000; }
bool Sleep(int64_t ticks) {
  if (ticks <= 0) return true;
  timespec left{time_t(ticks / 1000000000), long(ticks % 1000000000)};
  // A signal (the SDK's thread suspension) cuts the sleep short; the rest is slept.
  while (clock_nanosleep(CLOCK_MONOTONIC, 0, &left, &left) == EINTR) {
  }
  return true;
}
void Pause() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

void SetThreadPriority(ThreadPriority priority) {
  // Switching between these two needs no privilege; nice values stay as they are.
  const sched_param param{};
  pthread_setschedparam(pthread_self(), priority == ThreadPriority::kLowest ? SCHED_BATCH : SCHED_OTHER, &param);
}
bool ThreadTimes(void* native_thread, uint64_t& kernel_100ns, uint64_t& user_100ns) {
  // A pthread_t is valid until the SDK joins the thread, at the first wait
  // on an ended one: the caller asks only while the thread runs.
  if (!native_thread) return false;
  clockid_t clock{};
  timespec used{};
  if (pthread_getcpuclockid(reinterpret_cast<pthread_t>(native_thread), &clock) != 0 ||
      clock_gettime(clock, &used) != 0)
    return false;
  kernel_100ns = 0;
  user_100ns = uint64_t(used.tv_sec) * 10000000 + uint64_t(used.tv_nsec) / 100;
  return true;
}

// The window queries below run on the render thread, as their Win32 versions
// do, although SDL documents them for the main thread. On X11 and Wayland they
// read the state SDL keeps for the window, without a call to the display
// server. A client size kept by the UI thread would let them move there.
NativeWindow WindowHandles(void* native_handle) {
  NativeWindow window;
  auto* sdl = static_cast<SDL_Window*>(native_handle);
  if (!sdl) return window;
  window.handle = native_handle;
  const SDL_PropertiesID props = SDL_GetWindowProperties(sdl);
  if (void* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr)) {
    window.kind = NativeWindow::Kind::kWayland;
    window.display = display;
    window.window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
  } else if (void* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr)) {
    window.kind = NativeWindow::Kind::kXlib;
    window.display = display;
    window.window = reinterpret_cast<void*>(uintptr_t(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
  }
  if (!window.window) window = NativeWindow{};
  return window;
}
bool ClientSize(const NativeWindow& window, uint32_t& width, uint32_t& height) {
  auto* sdl = static_cast<SDL_Window*>(window.handle);
  int w = 0, h = 0;
  if (!sdl || (SDL_GetWindowFlags(sdl) & SDL_WINDOW_MINIMIZED) || !SDL_GetWindowSizeInPixels(sdl, &w, &h) || w <= 0 ||
      h <= 0)
    return false;
  width = uint32_t(w);
  height = uint32_t(h);
  return true;
}

std::string WindowFacts(const NativeWindow& window) {
  auto* sdl = static_cast<SDL_Window*>(window.handle);
  int w = 0, h = 0;
  if (sdl) SDL_GetWindowSizeInPixels(sdl, &w, &h);
  return fmt::format("\"client\":[{},{}],\"visible\":{},\"scale\":{},\"kind\":{}", w, h,
                     sdl && !(SDL_GetWindowFlags(sdl) & SDL_WINDOW_HIDDEN), sdl ? SDL_GetWindowDisplayScale(sdl) : 0.0f,
                     int(window.kind));
}
bool PostWindowClose(const NativeWindow& window) {
  auto* sdl = static_cast<SDL_Window*>(window.handle);
  if (!sdl) return false;
  SDL_Event event{};
  event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
  event.window.windowID = SDL_GetWindowID(sdl);
  return SDL_PushEvent(&event);
}

bool KeyDown(Key key) {
  const SDL_Scancode code = key == Key::kF6   ? SDL_SCANCODE_F6
                            : key == Key::kF7 ? SDL_SCANCODE_F7
                            : key == Key::kF8 ? SDL_SCANCODE_F8
                            : key == Key::kF9 ? SDL_SCANCODE_F9
                                              : SDL_SCANCODE_F10;
  int count = 0;
  const bool* state = SDL_GetKeyboardState(&count);
  return state && code < count && state[code];
}

const uintptr_t kPageSize = uintptr_t(sysconf(_SC_PAGESIZE));
bool ReadableRegion(const void* address, uintptr_t& begin, uintptr_t& end) {
  // One page (the protection is per page), probed by the kernel: process_vm_readv
  // fails with EFAULT where a load would fault, a stack's guard gap included,
  // and takes neither memory nor locks, so the crash report can call it inside
  // the signal handler. A sandbox that refuses the call falls back to the SDK's
  // /proc/self/maps reader.
  const uintptr_t page = reinterpret_cast<uintptr_t>(address) & ~(kPageSize - 1);
  char byte = 0;
  iovec local{&byte, 1}, remote{reinterpret_cast<void*>(page), 1};
  if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == 1) {
    begin = page;
    end = page + kPageSize;
    return true;
  }
  if (errno == EFAULT) return false;
  size_t length = 0;
  auto access = rex::memory::PageAccess::kNoAccess;
  if (!rex::memory::QueryProtect(reinterpret_cast<void*>(page), length, access) ||
      access == rex::memory::PageAccess::kNoAccess)
    return false;
  begin = page;
  end = page + length;
  return true;
}

std::filesystem::path ExecutablePath() {
  std::error_code error;
  auto path = std::filesystem::read_symlink("/proc/self/exe", error);
  return error ? std::filesystem::path() : path;
}
std::vector<std::string> LoadedModules() {
  std::vector<std::string> modules;
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) {
        auto& out = *static_cast<std::vector<std::string>*>(data);
        // The executable's own entry has an empty name.
        out.push_back(info->dlpi_name && *info->dlpi_name ? info->dlpi_name : ExecutablePath().string());
        return 0;
      },
      &modules);
  return modules;
}

void ShowError(std::string_view title, std::string_view text) {
  const std::string title_text(title), body(text);
  std::fprintf(stderr, "%s: %s\n", title_text.c_str(), body.c_str());
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title_text.c_str(), body.c_str(), nullptr);
}
void Terminate(uint32_t code) { _exit(int(code)); }
}  // namespace gpu::platform
