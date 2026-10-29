#include "sentry_reporting.h"
#include "diagnostics/incident.h"

#include <cstdlib>
#include <chrono>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <rex/logging.h>
#include <spdlog/sinks/rotating_file_sink.h>

#ifdef NARUTORISE_ENABLE_SENTRY
#include <sentry.h>
#endif

namespace narutorise::reporting {
namespace {
std::atomic<bool> initialized{false}, upload_session{false}, revoked{false};
#ifdef NARUTORISE_ENABLE_SENTRY
std::atomic<sentry_transaction_t*> startup{nullptr};

void CollectorLog(sentry_level_t level, const char* format, va_list arguments, void*) {
  char text[768]{};
  std::vsnprintf(text, sizeof(text), format, arguments);
  if (level >= SENTRY_LEVEL_WARNING || std::strstr(text, "WER"))
    incident::Event("collector-diagnostic", level >= SENTRY_LEVEL_WARNING ? "warning" : "info", text);
}

const char* Override(const char* name, const char* fallback) {
  const char* value = std::getenv(name);
  return value && *value ? value : fallback;
}
#endif
}  // namespace

bool Initialize(const std::filesystem::path& executable_dir,
                const std::filesystem::path& cache_root, const std::string& version,
                Consent consent) {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (initialized) return consent == Consent::kPending || SetConsent(consent == Consent::kGiven);
  // A pending answer keeps the destination: the SDK holds uploads until consent.
  const bool destination = consent != Consent::kRefused;
  try {
#ifdef _WIN32
    const auto handler = executable_dir / "crashpad_handler.exe";
#else
    const auto handler = executable_dir / "crashpad_handler";
#endif
    const auto local = incident::Directory();
    const auto database = local.empty() ? cache_root / "sentry-local" / std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count()) : local / "capture";
    if (!std::filesystem::is_regular_file(handler)) {
      REXLOG_WARN("[sentry] Crashpad handler missing; reporting unavailable");
      incident::Collector(false, "crashpad-handler-missing");
      return false;
    }
    std::filesystem::create_directories(database);
    auto* options = sentry_options_new();
    sentry_options_set_logger(options, CollectorLog, nullptr);
    sentry_options_set_logger_enabled_when_crashed(options, 0);
    sentry_options_set_debug(options, 1);
    // An empty DSN removes the destination as well as requiring SDK consent.
    sentry_options_set_dsn(options, destination ? Override("SENTRY_DSN", NARUTORISE_SENTRY_DSN) : "");
    const auto release = "narutorise@" + (version.empty() ? std::string("dev") : version)
                         + "+" + NARUTORISE_BUILD_REVISION;
    sentry_options_set_release(options, Override("SENTRY_RELEASE", release.c_str()));
    sentry_options_set_environment(options, Override("SENTRY_ENVIRONMENT", NARUTORISE_SENTRY_ENVIRONMENT));
    sentry_options_set_dist(options, NARUTORISE_BUILD_REVISION);
    sentry_options_set_require_user_consent(options, 1);
    sentry_options_set_traces_sample_rate(options, 0.1);
    sentry_options_set_shutdown_timeout(options, 2000);
    sentry_options_set_cache_keep(options, SENTRY_CACHE_KEEP_ALWAYS);
    sentry_options_set_cache_max_size(options, 32 * 1024 * 1024);
    sentry_options_set_cache_max_age(options, 14 * 24 * 60 * 60);
    // The guest threads commit their whole stacks (16 MiB): captured from the
    // TEB limits, a crash dump is ~250 MB, over the upload limit (rejected as
    // invalid multipart) and the local incident quota.
    sentry_options_set_crashpad_limit_stack_capture_to_sp(options, 1);
    if (destination && !local.empty()) {
#ifdef _WIN32
      sentry_options_add_attachmentw(options, (local / "channel.bin").c_str());
#else
      sentry_options_add_attachment(options, (local / "channel.bin").c_str());
#endif
    }
    if (const auto logger = rex::GetLogger()) {
      for (const auto& sink : logger->sinks()) {
        if (auto* file = dynamic_cast<spdlog::sinks::rotating_file_sink_mt*>(sink.get())) {
          const std::filesystem::path log(file->filename());
          incident::SetLog(log);
#ifdef _WIN32
          if (destination) sentry_options_add_attachmentw(options, log.c_str());
#else
          if (destination) sentry_options_add_attachment(options, log.c_str());
#endif
          break;
        }
      }
    }
#ifdef _WIN32
    sentry_options_set_handler_pathw(options, handler.c_str());
    sentry_options_set_database_pathw(options, database.c_str());
#else
    sentry_options_set_handler_path(options, handler.c_str());
    sentry_options_set_database_path(options, database.c_str());
#endif
    if (sentry_init(options) != 0) {
      REXLOG_WARN("[sentry] Initialization failed; continuing without reporting");
      incident::Collector(false, "sentry-init-failed");
      return false;
    }
    initialized = true;
    upload_session = destination; revoked = false;
    if (consent == Consent::kGiven) sentry_user_consent_give();
    else if (consent == Consent::kRefused) sentry_user_consent_revoke();
    incident::Collector(true, "Crashpad initialized synchronously; local capture independent of upload");
#ifdef _WIN32
    incident::SetMetadata("fast_fail_coverage", std::filesystem::is_regular_file(executable_dir / "crashpad_wer.dll")
        ? "WER sidecar present; registration diagnostics recorded; runtime coverage is not guaranteed"
        : "WER sidecar missing; fast-fail dump coverage unavailable");
#endif
    incident::SetMetadata("release", release);
    incident::SetMetadata("guest_register_policy", "published PPCContext; compiler-local values may differ; LR is not the native PC");
    incident::SetMetadata("session_id", incident::SessionId());
    const char* const transport[] = {"local-only", "pending", "authorized"};
    const char* const uploads[] = {"disabled", "pending consent", "enabled"};
    incident::Event("transport", transport[int(consent)], "database isolated per session; old sessions never reopened for upload");
    sentry_set_tag("application", "narutorise");
    sentry_set_tag("build_revision", NARUTORISE_BUILD_REVISION);
    sentry_set_tag("session_id", incident::SessionId().c_str());
    auto* context = sentry_transaction_context_new("Application startup", "app.start");
    startup = sentry_transaction_start(context, sentry_value_new_null());
    REXLOG_INFO("[sentry] Local capture ready; uploads {}; release {}; incident {}",
                uploads[int(consent)], release, incident::SessionId());
    return true;
  } catch (const std::exception& error) {
    REXLOG_WARN("[sentry] Reporting unavailable: {}", error.what());
    incident::Collector(false, error.what());
  }
#else
  (void)executable_dir; (void)cache_root; (void)version; (void)consent;
  REXLOG_INFO("[sentry] Reporting not compiled into this build");
  incident::Collector(false, "sentry-not-compiled");
#endif
  return false;
}

bool SetConsent(bool consent) {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (initialized) {
    if (consent && (!upload_session || revoked)) return false; // New session/restart required.
    if (consent && sentry_user_consent_get() != SENTRY_USER_CONSENT_GIVEN) {
      sentry_user_consent_give();
      incident::Event("transport", "authorized", "given during the session; uploads from this session only");
    }
    if (!consent) {
      revoked = true;
      sentry_user_consent_revoke();
      incident::Event("transport", "revoked", "new uploads disabled; in-flight upload may already have arrived; re-enable requires a new session");
    }
    return true;
  }
#endif
  return false;
}

void FinishStartup() {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (auto* transaction = startup.exchange(nullptr)) sentry_transaction_finish(transaction);
#endif
}

void Close() {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (!initialized.exchange(false)) return;
  FinishStartup();
  sentry_close();
#endif
}

bool CaptureFailure(const std::string& operation, const std::string& evidence) {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (!initialized || revoked || sentry_user_consent_get() != SENTRY_USER_CONSENT_GIVEN) return false;
  auto event = sentry_value_new_event();
  sentry_value_set_by_key(event, "level", sentry_value_new_string("error"));
  const auto message = "Native renderer failure: " + operation;
  sentry_value_set_by_key(event, "message", sentry_value_new_string(message.c_str()));
  auto context = sentry_value_new_object();
  sentry_value_set_by_key(context, "operation", sentry_value_new_string(operation.c_str()));
  sentry_value_set_by_key(context, "evidence", sentry_value_new_string(evidence.substr(0, 4096).c_str()));
  auto contexts = sentry_value_new_object(); sentry_value_set_by_key(contexts, "renderer_failure", context);
  sentry_value_set_by_key(event, "contexts", contexts);
  const auto id = sentry_capture_event(event);
  char uuid[37]{}; sentry_uuid_as_string(&id, uuid);
  incident::SetMetadata("remote_event_id", uuid);
  // Queued is distinct from accepted. The local incident is already durable
  // before this optional, bounded transport wait.
  sentry_flush(500);
  return !sentry_uuid_is_nil(&id);
#else
  (void)operation; (void)evidence; return false;
#endif
}

bool Verify(const std::string& mode) {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (!initialized || sentry_user_consent_get() != SENTRY_USER_CONSENT_GIVEN) return false;
  if (mode == "crash") {
    REXLOG_ERROR("[sentry] Triggering requested verification crash (abort)");
    rex::FlushLogging();
    std::abort();
  }
  if (mode != "error") return false;
  try {
    throw std::runtime_error("Naruto Rise Sentry verification error");
  } catch (const std::runtime_error& error) {
    auto event = sentry_value_new_event();
    sentry_value_set_by_key(event, "level", sentry_value_new_string("error"));
    auto exception = sentry_value_new_exception("std::runtime_error", error.what());
    sentry_value_set_stacktrace(exception, nullptr, 0);
    sentry_event_add_exception(event, exception);
    sentry_set_tag("setup_verification", "true");
    const auto id = sentry_capture_event(event);
    char uuid[37]; sentry_uuid_as_string(&id, uuid);
    REXLOG_INFO("[sentry] Verification event {}: {}", uuid, error.what());
    return !sentry_uuid_is_nil(&id);
  }
#else
  (void)mode;
  return false;
#endif
}
}  // namespace narutorise::reporting
