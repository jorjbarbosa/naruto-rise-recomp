#pragma once

// Hooks on recompiled functions. Recompiled functions are weak aliases (sub_XXXXXXXX) of their
// implementation (__imp__sub_XXXXXXXX), so a definition with PPC_FUNC(sub_XXXXXXXX) replaces every
// call to them, direct or through the function table. The hook runs the original through ORIGINAL:
// DECLARE_ORIGINAL(D3D_RESOLVE); ... ORIGINAL(D3D_RESOLVE)(ctx, base);
#define ORIGINAL(name) ORIGINAL_EXPANDED(name)
#define ORIGINAL_EXPANDED(name) __imp__##name
#define DECLARE_ORIGINAL(name) extern "C" PPC_FUNC(ORIGINAL(name))
