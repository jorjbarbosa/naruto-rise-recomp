// Crash evidence of a host crash (crash_dump.h). The SDK's vectored
// handler logs "Unhandled guest access violation" and continues the search, so
// the process reaches the unhandled-exception filter installed here, which
// writes a full minidump without a debugger attached (the crash did not
// reproduce in 9 minutes under CDB: it is a timing race). The ring of the
// title's frees (820C0018, the engine free that reaches dlfree 82188C88 and
// unlink_large_chunk 82187D58, where the corruption is detected) names the
// thread, the lr and the time of the last 1024 frees, to match the corrupted
// chunk of the dump with the free that released it; the hook of that free
// lives in engine_free.cpp.
#include "gpu/diagnostics/crash_dump.h"
#include "diagnostics/incident.h"
#include "gpu/diagnostics/heap_watch.h"
#include <windows.h>
#include <dbghelp.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace gpu::crash_dump {
namespace {
struct FreeEntry {
  uint32_t kind, pointer, heap, arg4, lr, guest_thread;
  int64_t qpc;
};
constexpr uint32_t kRing = 1024;
std::array<FreeEntry, kRing> ring{};
std::atomic<uint32_t> ring_count{0};
bool installed = false;
std::string directory;

const char* Directory() {
  if (directory.empty()) {
    const char* dir = std::getenv("REX_NATIVE_FRAME_DIR");
    directory = dir && *dir ? dir : ".";
  }
  return directory.c_str();
}

void WriteFrees(const char* path) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  const uint32_t count = ring_count.load();
  const uint32_t first = count > kRing ? count - kRing : 0;
  std::fprintf(f, "{\"frees\":%u,\"entries\":[", count);
  for (uint32_t i = first; i < count; ++i) {
    const FreeEntry& e = ring[i % kRing];
    std::fprintf(f, "%s{\"n\":%u,\"kind\":%u,\"pointer\":%u,\"heap\":%u,\"arg4\":%u,\"lr\":%u,\"guest_thread\":%u,"
                 "\"qpc\":%lld}", i == first ? "" : ",", i, e.kind, e.pointer, e.heap, e.arg4, e.lr, e.guest_thread,
                 static_cast<long long>(e.qpc));
  }
  std::fprintf(f, "]}\n");
  std::fclose(f);
}

LONG WINAPI Filter(EXCEPTION_POINTERS* pointers) {
  static std::atomic<int> entered{0};
  if (entered.fetch_add(1)) return EXCEPTION_CONTINUE_SEARCH;
  const std::string dump = std::string(Directory()) + "/crash.dmp";
  const std::string frees = std::string(Directory()) + "/crash-frees.json";
  WriteFrees(frees.c_str());
  heap_watch::WriteReport((std::string(Directory()) + "/crash-heap-watch.json").c_str());
  using Writer = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                              PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
  HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
  auto writer = dbghelp ? reinterpret_cast<Writer>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
  HANDLE file = CreateFileA(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  BOOL written = FALSE;
  if (writer && file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), pointers, FALSE};
    written = writer(GetCurrentProcess(), GetCurrentProcessId(), file,
                     MINIDUMP_TYPE(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                   MiniDumpWithFullMemoryInfo),
                     &info, nullptr, nullptr);
  }
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  char text[512];
  std::snprintf(text, sizeof(text), "native crash dump: code 0x%08lX address %p written=%d path=%s\n",
                pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionCode : 0ul,
                pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionAddress : nullptr,
                int(written), dump.c_str());
  OutputDebugStringA(text);
  std::fputs(text, stderr);
  return EXCEPTION_CONTINUE_SEARCH;  // Windows ends the process; the evidence is on disk.
}
}  // namespace

void Install() {
  const char* text = std::getenv("REX_NATIVE_CRASH_DUMP");
  if (!text || std::string_view(text) != "1" || installed) return;
  if (narutorise::incident::CollectorReady()) {
    narutorise::incident::Event("legacy-full-dump", "skipped", "Crashpad owns the fatal exception filter");
    return;
  }
  SetUnhandledExceptionFilter(Filter);
  installed = true;
}

void NoteFree(uint32_t kind, uint32_t pointer, uint32_t heap, uint32_t arg4, uint32_t lr, uint32_t guest_thread) {
  if (!installed) return;
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  const uint32_t n = ring_count.fetch_add(1);
  ring[n % kRing] = FreeEntry{kind, pointer, heap, arg4, lr, guest_thread, now.QuadPart};
}
}  // namespace gpu::crash_dump
