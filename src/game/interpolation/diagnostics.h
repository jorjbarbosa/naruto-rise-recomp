#pragma once

// Output files of the state interpolation probes: <exe>/diagnostics/<name>.

#include <cstdio>

namespace game::interpolation
{
    // Opens <exe>/diagnostics/<name> for writing (truncated), creating the folder. Logs and returns
    // nullptr when it cannot. The caller closes it. Any thread.
    std::FILE* OpenDiagnosticsFile(const char* name);

    // Whether an environment variable is set to "1".
    bool EnvironmentFlag(const char* name);

    // Whether an environment variable is set to "0": a part on by default turned off.
    bool EnvironmentOff(const char* name);
}
