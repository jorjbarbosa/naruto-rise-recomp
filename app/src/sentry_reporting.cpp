#include "sentry_reporting.h"

#include <cstdlib>
#include <stdexcept>
#include <rex/logging.h>

#ifdef NARUTORISE_ENABLE_SENTRY
#include <sentry.h>
#endif

namespace narutorise::reporting {
namespace {
bool initialized = false;
#ifdef NARUTORISE_ENABLE_SENTRY
sentry_transaction_t* startup = nullptr;

const char* Override(const char* name, const char* fallback) {
  const char* value = std::getenv(name);
  return value && *value ? value : fallback;
}
#endif
}  // namespace

bool Initialize(const std::filesystem::path& executable_dir,
                const std::filesystem::path& cache_root, const std::string& version,
                bool consent) {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (initialized) return SetConsent(consent);
  if (!consent) {
    REXLOG_INFO("[sentry] Reporting disabled; no background handler or uploads");
    return false;
  }
  try {
#ifdef _WIN32
    const auto handler = executable_dir / "crashpad_handler.exe";
#else
    const auto handler = executable_dir / "crashpad_handler";
#endif
    const auto database = cache_root / "sentry";
    if (!std::filesystem::is_regular_file(handler)) {
      REXLOG_WARN("[sentry] Crashpad handler missing; reporting unavailable");
      return false;
    }
    std::filesystem::create_directories(database);
    auto* options = sentry_options_new();
    sentry_options_set_dsn(options, Override("SENTRY_DSN", NARUTORISE_SENTRY_DSN));
    const auto release = "narutorise@" + (version.empty() ? std::string("dev") : version)
                         + "+" + NARUTORISE_BUILD_REVISION;
    sentry_options_set_release(options, Override("SENTRY_RELEASE", release.c_str()));
    sentry_options_set_environment(options, Override("SENTRY_ENVIRONMENT", NARUTORISE_SENTRY_ENVIRONMENT));
    sentry_options_set_dist(options, NARUTORISE_BUILD_REVISION);
    sentry_options_set_require_user_consent(options, 1);
    sentry_options_set_traces_sample_rate(options, 0.1);
    sentry_options_set_shutdown_timeout(options, 2000);
#ifdef _WIN32
    sentry_options_set_handler_pathw(options, handler.c_str());
    sentry_options_set_database_pathw(options, database.c_str());
#else
    sentry_options_set_handler_path(options, handler.c_str());
    sentry_options_set_database_path(options, database.c_str());
#endif
    if (sentry_init(options) != 0) {
      REXLOG_WARN("[sentry] Initialization failed; continuing without reporting");
      return false;
    }
    initialized = true;
    sentry_user_consent_give();
    sentry_set_tag("application", "narutorise");
    sentry_set_tag("build_revision", NARUTORISE_BUILD_REVISION);
    auto* context = sentry_transaction_context_new("Application startup", "app.start");
    startup = sentry_transaction_start(context, sentry_value_new_null());
    REXLOG_INFO("[sentry] Reporting enabled; release {}", release);
    return true;
  } catch (const std::exception& error) {
    REXLOG_WARN("[sentry] Reporting unavailable: {}", error.what());
  }
#else
  (void)executable_dir; (void)cache_root; (void)version; (void)consent;
  REXLOG_INFO("[sentry] Reporting not compiled into this build");
#endif
  return false;
}

bool SetConsent(bool consent) {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (initialized) {
    if (consent) sentry_user_consent_give();
    else sentry_user_consent_revoke();
    return true;
  }
#endif
  return false;
}

void FinishStartup() {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (startup) {
    sentry_transaction_finish(startup);
    startup = nullptr;
  }
#endif
}

void Close() {
#ifdef NARUTORISE_ENABLE_SENTRY
  if (!initialized) return;
  FinishStartup();
  sentry_close();
  initialized = false;
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
