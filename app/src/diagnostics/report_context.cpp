// The facts of the session for the crash reports (report_context.h).
#include "diagnostics/report_context.h"
#include "diagnostics/incident.h"
#include "diagnostics/log_facts.h"
#include "sentry_reporting.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/system/xthread.h>
#include <spdlog/sinks/base_sink.h>

#ifdef NARUTORISE_ENABLE_SENTRY
#include <sentry.h>
#endif

namespace narutorise::report_context {
namespace {
// Sentry's limits: a tag value takes 200 characters; context values are cut
// like CaptureFailure's evidence.
constexpr size_t kTagMax = 200, kContextMax = 4096;

struct State {
  std::mutex mutex;
  std::map<std::string, std::string> tags;
  std::map<std::string, std::map<std::string, std::string>> contexts;
  std::vector<std::string> devkit_modules;
  std::string fatal;  // Normalized, for the fingerprint.
};
State& Facts() {
  static State state;
  return state;
}

thread_local const char* thread_role = nullptr;

std::string Join(const std::vector<std::string>& items, size_t max) {
  std::string out;
  for (const auto& item : items) {
    if (!out.empty()) out += ", ";
    out += item;
  }
  if (out.size() > max) out.resize(max);
  return out;
}

void ApplyTag(const std::string& key, const std::string& value) {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (reporting::Ready()) sentry_set_tag(key.c_str(), value.c_str());
#else
  (void)key; (void)value;
#endif
}

void ApplyContext(const std::string& name, const std::map<std::string, std::string>& fields) {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (!reporting::Ready()) return;
  auto object = sentry_value_new_object();
  for (const auto& [key, value] : fields) sentry_value_set_by_key(object, key.c_str(), sentry_value_new_string(value.c_str()));
  sentry_set_context(name.c_str(), object);
#else
  (void)name; (void)fields;
#endif
}

void ApplyFingerprint(const std::string& fatal) {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (reporting::Ready() && !fatal.empty()) sentry_set_fingerprint("fatal", fatal.c_str(), nullptr);
#else
  (void)fatal;
#endif
}

// Under the facts' mutex.
void Tag(State& state, std::string key, std::string value) {
  if (value.size() > kTagMax) value.resize(kTagMax);
  incident::SetMetadata("tag." + key, value);
  ApplyTag(key, value);
  state.tags[std::move(key)] = std::move(value);
}

void Context(State& state, const std::string& name, std::map<std::string, std::string> fields) {
  auto& context = state.contexts[name];
  for (auto& [key, value] : fields) {
    if (value.size() > kContextMax) value.resize(kContextMax);
    incident::SetMetadata(name + "." + key, value);
    context[key] = std::move(value);
  }
  ApplyContext(name, context);
}

void NoteFatal(const std::string& message) {
  auto& state = Facts();
  std::lock_guard lock(state.mutex);
  state.fatal = log_facts::NormalizeFatal(message);
  Tag(state, "fatal", state.fatal);
  Context(state, "fatal", {{"message", message}});
  ApplyFingerprint(state.fatal);
}

void NoteDevkitModule(const std::string& module) {
  auto& state = Facts();
  std::lock_guard lock(state.mutex);
  state.devkit_modules.push_back(module);
  Tag(state, "dlc_devkit", Join(state.devkit_modules, kTagMax));
  Context(state, "game", {{"devkit_modules", Join(state.devkit_modules, kContextMax)}});
}

// The SDK's log, synchronous: a FATAL line reaches the sink before its abort.
class LogSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& message) override {
    if (message.level < spdlog::level::warn && !reader_.WantsInfo()) return;
    const auto fact = reader_.Feed(std::string_view(message.payload.data(), message.payload.size()));
    if (fact.fatal) NoteFatal(*fact.fatal);
    if (fact.devkit_module) NoteDevkitModule(*fact.devkit_module);
  }
  void flush_() override {}

 private:
  log_facts::Reader reader_;
};

// REX_NATIVE_SENTRY_FAULT=<role>: a read of an unmapped address once the
// named role starts, to verify a crash report from that thread.
void TestFault(const char* role) {
  static const char* wanted = std::getenv("REX_NATIVE_SENTRY_FAULT");
  static std::atomic<bool> faulted{false};
  if (!wanted || std::strcmp(wanted, role) != 0 || faulted.exchange(true)) return;
  REXLOG_ERROR("[sentry] Triggering requested verification fault on the {} thread", role);
  rex::FlushLogging();
  const auto* unmapped = reinterpret_cast<volatile const uint32_t*>(uintptr_t(0x28));
  (void)*unmapped;
}
}  // namespace

void SetGraphics(std::string_view renderer, std::string_view backend, int frame_rate, double resolution_scale,
                 int msaa_samples) {
  auto& state = Facts();
  std::lock_guard lock(state.mutex);
  const std::string scale = resolution_scale > 0.0 ? fmt::format("{:g}", resolution_scale) : "auto";
  Tag(state, "renderer", std::string(renderer));
  Tag(state, "backend", std::string(backend));
  Tag(state, "frame_rate", std::to_string(frame_rate));
  Tag(state, "resolution_scale", scale);
  Tag(state, "msaa", std::to_string(msaa_samples));
  Context(state, "graphics", {{"renderer", std::string(renderer)}, {"backend_requested", std::string(backend)},
                              {"frame_rate", std::to_string(frame_rate)}, {"resolution_scale", scale},
                              {"msaa_samples", std::to_string(msaa_samples)}});
}

void SetRenderMode(double resolution_scale, int msaa_samples) {
  auto& state = Facts();
  std::lock_guard lock(state.mutex);
  const std::string scale = fmt::format("{:g}", resolution_scale);
  Tag(state, "resolution_scale", scale);
  Tag(state, "msaa", std::to_string(msaa_samples));
  Context(state, "graphics", {{"resolution_scale", scale}, {"msaa_samples", std::to_string(msaa_samples)}});
}

void SetRuntime(const char* wine_version) {
  auto& state = Facts();
  std::lock_guard lock(state.mutex);
  Tag(state, "runtime", wine_version ? "wine" : "native");
  if (wine_version) Context(state, "runtime", {{"wine_version", wine_version}});
}

void SetGameData(bool disc_image, uint32_t title_id, std::string_view xex_digest) {
  auto& state = Facts();
  std::lock_guard lock(state.mutex);
  Tag(state, "game_data", disc_image ? "iso" : "folder");
  Context(state, "game", {{"title_id", fmt::format("{:08X}", title_id)}, {"xex_xxh3", std::string(xex_digest)},
                          {"data", disc_image ? "iso" : "folder"}});
}

void SetContent(const std::vector<std::string>& packages) {
  auto& state = Facts();
  std::lock_guard lock(state.mutex);
  Tag(state, "dlc_count", std::to_string(packages.size()));
  if (state.devkit_modules.empty()) Tag(state, "dlc_devkit", "none");
  Context(state, "game", {{"dlc", Join(packages, kContextMax)}});
}

void SetAdapter(std::string_view api, std::string_view name, uint32_t vendor_id, uint32_t device_id,
                std::string_view driver) {
  const char* vendor = vendor_id == 0x10DE ? "NVIDIA" : vendor_id == 0x1002 ? "AMD" : vendor_id == 0x8086 ? "Intel"
                       : "other";
  auto& state = Facts();
  std::lock_guard lock(state.mutex);
  Tag(state, "backend", std::string(api));
  Context(state, "gpu", {{"name", std::string(name)}, {"vendor_name", vendor},
                         {"vendor_id", fmt::format("0x{:04X}", vendor_id)}, {"id", fmt::format("0x{:04X}", device_id)},
                         {"version", std::string(driver)}, {"api_type", std::string(api)}});
}

void SetThreadRole(const char* role) {
  if (thread_role) return;
  thread_role = role;
  if (rex::system::XThread::IsInThread())
    rex::system::XThread::GetCurrentThread()->set_name(fmt::format("Guest {}", role));
  TestFault(role);
}

const char* ThreadRole() noexcept { return thread_role ? thread_role : "other"; }

void InstallLogSink() {
  static std::once_flag installed;
  std::call_once(installed, [] { rex::AddSink(std::make_shared<LogSink>()); });
}

void Publish() {
  auto& state = Facts();
  std::lock_guard lock(state.mutex);
  for (const auto& [key, value] : state.tags) ApplyTag(key, value);
  for (const auto& [name, fields] : state.contexts) ApplyContext(name, fields);
  ApplyFingerprint(state.fatal);
}
}  // namespace narutorise::report_context
