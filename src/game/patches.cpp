// Game patches: replacements for recompiled functions of default.xex. Recompiled functions are
// weak aliases (sub_XXXXXXXX) of their implementation (__imp__sub_XXXXXXXX), so a definition here
// replaces every call to them.

#include "kernel/kernel.h"

// XFileCacheInit (XAPI): the file cache copies DVD files to the hard disk cache partition and then
// serves reads from there. It has no use on PC and there is no cache partition, so report a
// failure: the game then skips the whole cache setup (sub_82686060) and reads its files directly
// from game:\.
PPC_FUNC(sub_82176D30)
{
    TRACE("XFileCacheInit refused");
    ctx.r3.u64 = kernel::X_ERROR_FUNCTION_FAILED;
}
