#pragma once

// State and services shared by the units of the host of the native mode
// (host/*.cpp): the journal, the ordered stop, the engine owner, the graphics
// system and the frame completion thread. The D3D boundary sees host/services.h.
#include "gpu/host/services.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <rex/cvar.h>
#include <rex/ppc/context.h>

REXCVAR_DECLARE(bool, native_renderer_bootstrap);
REXCVAR_DECLARE(std::string, native_renderer_bootstrap_path);

namespace rex::system { class IGraphicsSystem; }
namespace gpu { class Dispatcher; }

namespace gpu::host {
// The native mode is on (host/host.cpp, Configure).
extern std::atomic<bool> enabled;

// The journal (host/journal.cpp): JSONL records under event_mutex, the loaded
// modules and the provenance written at the stop. Without a journal path
// (the product) nothing is written.
extern std::mutex event_mutex;
extern uint64_t sequence;
bool OpenJournal(const std::string& path);
void Write(std::string_view text);
void Record(std::string_view kind, std::string_view name, PPCContext* ctx, uint32_t address = 0, uint32_t value = 0);
void Modules();
extern std::atomic<uint64_t> wait_records, last_error_records;
bool KeepRecord(std::atomic<uint64_t>& counter);
void WriteStopProvenance(int code);

// The ordered stop (host/stop.cpp).
void ClaimStop(std::string_view name, PPCContext* ctx);
bool ReleaseEngineAndRecord(std::string_view name, PPCContext* ctx, uint32_t address, uint32_t value,
                            bool stop_record, std::string* shutdown_evidence = nullptr);
[[noreturn]] void Stop(std::string_view name, PPCContext* ctx, uint32_t address = 0, uint32_t value = 0,
                       int code = 86);
PPCContext* Context();

// The engine owner (host/engines.cpp). Owned values only: no retained
// PPCContext, translated memory or XThread ptr.
struct EngineRegistration {
  uint32_t callback, argument, guest_thread;
  int cpu;
  uint32_t device;
};
extern std::mutex engine_mutex;
extern std::unique_ptr<Dispatcher> native_engine;
// native_engine.get(), stored under engine_mutex where native_engine changes:
// ActiveDispatcher reads it without the lock (twice per draw).
extern std::atomic<Dispatcher*> active_engine;
extern std::unique_ptr<Dispatcher> failed_native_engine;
extern EngineRegistration engine_registration;
bool InitializeEngines(PPCContext& ctx);

// The graphics system of the native mode (host/graphics.cpp).
std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();
std::string InterruptRegistrationEvidence();
void ValidateInterruptRegistration(PPCContext& ctx);

// The frame completion thread (host/frame_completion.cpp).
namespace frame_completion {
extern std::atomic<bool> started, closing;
void Quiesce();
std::string Summary();
}  // namespace frame_completion
}  // namespace gpu::host
