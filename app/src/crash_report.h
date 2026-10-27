// narutorise - ReXGlue Recompiled Project
//
// The SDK fault chain publishes a fixed guest context for the external incident
// collector, without a guest stack/heap walk, allocation or logging in the
// exception callback. Crashpad preserves the original native exception context.
// The ordinary transform-cycle probe (8266AE68) and world-load log (8264C358)
// remain independent of the fatal capture path.

#pragma once

namespace narutorise::crash_report {
// Installs the handler once; from OnPostSetup, after the SDK's own handler.
void Install();
}  // namespace narutorise::crash_report
