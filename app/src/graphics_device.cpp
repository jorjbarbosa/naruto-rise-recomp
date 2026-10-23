// narutorise - graphics card choice (graphics_device.h): the option shared by
// both renderers. The Xenos side is in graphics_device_win32.cpp.

#include "graphics_device.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(graphics_device, "", "narutorise/Graphics",
                      "Graphics card by its name as the system lists it (empty = automatic)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
