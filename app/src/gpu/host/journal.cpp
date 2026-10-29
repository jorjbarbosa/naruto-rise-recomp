// The journal of the native mode: JSONL records of every host and guest event
// under one writer lock, the loaded modules, the capped high-frequency records
// (guest waits, SetLastError) and the provenance that the stop writes.
#include "gpu/host/internal.h"
#include "gpu/common/platform.h"
#include "gpu/common/sha256.h"
#include "gpu/d3d/d3d.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

namespace gpu::host {
std::mutex event_mutex;
uint64_t sequence = 0;
namespace {
std::FILE* events = nullptr;
// SHA-256 and size of every byte written to the journal, finished by the
// stop provenance after the last line (under the final writer lock).
std::unique_ptr<Sha256> events_hash;
uint64_t events_bytes = 0;
std::mutex provenance_mutex;
std::string stop_provenance;  // Fields of the live START provenance.
const auto start = std::chrono::steady_clock::now();
}  // namespace

bool Journaled() { return events != nullptr; }

// A new file only ("x": the journal of a run is never overwritten), without a
// buffer of our own.
bool OpenJournal(const std::string& path) {
  events = std::fopen(std::filesystem::path(path).string().c_str(), "wbx");
  if (!events) return false;
  std::setvbuf(events, nullptr, _IONBF, 0);
  events_hash = std::make_unique<Sha256>();
  return true;
}

// No disk flush per record: every guest wait and SetLastError records under
// event_mutex, and a flush per line (about 1.2 ms) would starve the title's
// main thread while it loads. The unbuffered write still lands in the system
// cache before returning, which survives the process exit codes 86/88.
void Write(std::string_view text) {
  if (!events) return;
  if (std::fwrite(text.data(), 1, text.size(), events) != text.size()) std::_Exit(88);
  if (events_hash) events_hash->Update(text.data(), text.size());
  events_bytes += text.size();
}

void Record(std::string_view kind, std::string_view name, PPCContext* ctx,
            uint32_t address, uint32_t value) {
  if (!events) return;
  auto* ts = rex::runtime::ThreadState::Get();
  auto* thread = rex::system::XThread::GetCurrentThread();
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start).count();
  Write(fmt::format("{{\"sequence\":{},\"elapsed_us\":{},\"kind\":\"{}\",\"name\":\"{}\","
      "\"host_thread\":{},\"guest_thread\":{},\"cpu\":{},\"context_present\":{},"
      "\"lr\":{},\"ctr\":{},\"r1\":{},\"r13\":{},\"args\":[{},{},{},{},{},{},{},{}],"
      "\"address\":{},\"value\":{}}}\n",
      ++sequence, elapsed, kind, name, rex::thread::current_thread_system_id(), ts ? ts->thread_id() : 0,
      thread ? int(thread->active_cpu()) : -1, ctx != nullptr,
      ctx ? ctx->lr : 0, ctx ? ctx->ctr.u64 : 0, ctx ? ctx->r1.u32 : 0,
      ctx ? ctx->r13.u32 : 0, ctx ? ctx->r3.u64 : 0, ctx ? ctx->r4.u64 : 0,
      ctx ? ctx->r5.u64 : 0, ctx ? ctx->r6.u64 : 0, ctx ? ctx->r7.u64 : 0,
      ctx ? ctx->r8.u64 : 0, ctx ? ctx->r9.u64 : 0, ctx ? ctx->r10.u64 : 0,
      address, value));
}

void Modules() {
  if (!events) return;
  const auto modules = platform::LoadedModules();
  if (modules.empty()) std::_Exit(88);
  for (const auto& module : modules) {
    std::string path;
    // JSON escaping; module paths on this machine use ASCII characters.
    for (const char c : module) {
      const auto byte = static_cast<unsigned char>(c);
      if (byte > 127 || byte < 32) std::_Exit(88);
      if (c == '\\' || c == '"') path += '\\';
      path += c;
    }
    Write(fmt::format("{{\"sequence\":{},\"kind\":\"module\",\"path\":\"{}\"}}\n",
                      ++sequence, path));
  }
}

void Record(std::string_view kind, std::string_view fields) {
  if (!events) return;
  std::lock_guard lock(event_mutex);
  Write(fmt::format("{{\"sequence\":{},\"kind\":\"{}\",{}}}\n", ++sequence, kind, fields));
}
// In the native mode the frame loop runs and every guest wait and
// SetLastError would be a line (about 48,000 per second once the writer stopped
// flushing). The first kRecordCap of each are written; the rest are only
// counted (RecordSummary). The hooks still run their checks on every call.
constexpr uint64_t kRecordCap = 4096;
std::atomic<uint64_t> wait_records{0}, last_error_records{0};
bool KeepRecord(std::atomic<uint64_t>& counter) {
  return counter.fetch_add(1) < kRecordCap || !gpu::Active();
}
std::string RecordSummary() {
  return fmt::format("{{\"waits\":{},\"last_error\":{},\"cap_each\":{}}}", wait_records.load(),
                     last_error_records.load(), kRecordCap);
}

// Identities that the process itself records for the live comparator,
// written after the last journal line under the final writer lock: SHA-256 of
// the journal bytes, of the executable and of the increment source identity
// (REX_NATIVE_SOURCE_IDENTITY), with the fields of the accepted occurrence.
// Nothing here opens a reference artifact.
namespace {
std::string JsonPath(const std::filesystem::path& path) {
  std::string out;
  for (const char c : path.string()) {
    if (c < 32 || c > 126) { out += '?'; continue; }
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}
}  // namespace
void WriteStopProvenance(int code) {
  std::string fields;
  {
    std::lock_guard lock(provenance_mutex);
    fields = stop_provenance;
  }
  const char* dir = std::getenv("REX_NATIVE_FRAME_DIR");
  if (fields.empty() || !dir || !*dir) return;
  const std::filesystem::path exe_path = platform::ExecutablePath();
  const char* identity = std::getenv("REX_NATIVE_SOURCE_IDENTITY");
  const std::filesystem::path identity_path = identity && *identity ? std::filesystem::path(identity)
                                                                     : std::filesystem::path();
  const std::filesystem::path events_path(REXCVAR_GET(native_renderer_bootstrap_path));
  const std::string text = fmt::format(
      "{{\"schema\":\"native-start-live-provenance-v1\",\"written_by\":\"process-at-stop\",\"exit_code\":{},"
      "\"executable\":{{\"path\":\"{}\",\"sha256\":\"{}\"}},\"source_identity\":{{\"path\":\"{}\",\"sha256\":\"{}\"}},"
      "\"events\":{{\"path\":\"{}\",\"bytes\":{},\"last_sequence\":{},\"sha256\":\"{}\"}},{}}}\n", code,
      JsonPath(exe_path), exe_path.empty() ? "" : gpu::Sha256File(exe_path), JsonPath(identity_path),
      identity_path.empty() ? "" : gpu::Sha256File(identity_path), JsonPath(events_path), events_bytes, sequence,
      events_hash ? events_hash->Finish() : "", fields);
  const std::filesystem::path out = std::filesystem::path(dir) / "live-provenance.json";
  std::FILE* file = std::fopen(out.string().c_str(), "wbx");
  if (!file) return;
  std::fwrite(text.data(), 1, text.size(), file);
  std::fclose(file);
}
void SetStopProvenance(std::string_view fields) {
  std::lock_guard lock(provenance_mutex);
  stop_provenance = fields;
}
}  // namespace gpu::host
