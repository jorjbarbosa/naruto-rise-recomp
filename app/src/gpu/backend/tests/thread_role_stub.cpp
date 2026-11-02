// The engine tests do not link the crash reports (diagnostics/report_context.cpp,
// with Sentry): the dispatcher's thread names go nowhere there.
#include "diagnostics/report_context.h"

namespace narutorise::report_context {
void SetThreadRole(const char*) {}
}  // namespace narutorise::report_context
