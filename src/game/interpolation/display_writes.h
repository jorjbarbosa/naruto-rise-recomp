#pragma once

// State the display writes from what it is given between ticks (state interpolation plan, section 8.8
// and invariant 2; step 6 document of the plan's folder).
//
// Under the gate every view block draws the objects and the camera between two ticks (transforms.h),
// the tick's own included (it shows the previous tick). Part of what the display computes from those
// matrices stays in the objects after the view block, where the next tick's logic reads it. The
// transforms put back the floats they stored; what the display computed from them is put here at what
// the display would have computed from the tick's state, by running the engine's own function again
// right after the transforms' restore (Settle), with the tick's matrices in place:
// - a hierarchy object whose global matrix the display computed from its father (the Apply of
//   modifiers 5, 14, 22 and 45: JADE_GLOBAL_FROM_HIERARCHY into its [object+20], or a change of the
//   global's rows or position before JADE_LOCAL_FROM_GLOBAL): the global again through the hierarchy,
//   from the local the display left and the fathers' tick matrices. The local keeps what the display
//   made of it: modifier 5's turn to the camera of the frame (plan section 8.8, "keep the display's
//   part");
// - a hierarchy object whose local the display recomputed from a global it did not move (modifier 26
//   scales it; the skeleton walk for objects with jade::object::kStatusGlobalToBone): the local again
//   from the global and the father's tick matrix (JADE_LOCAL_FROM_GLOBAL);
// - an object of an animated object's skeleton: the skeleton walk again (JADE_SKELETON_CHILD, with the
//   arguments the display gave it), so its bone and its global (or its local) come from the animated
//   object's tick matrix.
// Locals go first, fathers before children, then globals, then the skeleton objects.
// The image is not touched: the next view block computes all of it again from its own matrices.
//
// Left as the display writes them (plan section 8.8, "Leave it"; step 6 document): the display data's
// camera (only the display reads it), what modifiers 1, 47, 53, 56 and 59 compute from the camera (shader
// parameters, a fade by the dt), per-display counters, caches and buffers. What the display integrates by
// the dt (modifiers 37, 42, 45, the material animators) is the view clock's (view_clock.h).
//
// ROAN_DISPLAY_WRITES=0 turns the settling off under the gate (for a comparison). Main thread only: the
// calls come from engine_hooks.cpp and the transforms, inside the main step.

#include <ppc_config.h>
#include <ppc_context.h>

#include <cstdint>

namespace game::interpolation::display_writes
{
    bool Enabled();

    // The view setup, after the transforms': whether the view block is under the gate with the
    // transforms on. Drops the notes of a view block that did not settle.
    void BlockStart(bool active);

    // Inside the display of a block under the gate:
    // - the display renders an object (JADE_RENDER_OBJECT, before its modifiers apply);
    void NoteRendered(uint32_t object);
    // - JADE_LOCAL_FROM_GLOBAL (r3), called from caller (the guest's return address);
    void LocalFromGlobal(uint32_t object, uint32_t caller);
    // - JADE_GLOBAL_FROM_HIERARCHY (r3, r4);
    void GlobalFromHierarchy(uint32_t object, uint32_t out);
    // - around JADE_SKELETON_CHILD (r3 child, r4 animated object, r5 bone).
    void SkeletonChildStart(uint32_t child, uint32_t object, uint32_t bone);
    void SkeletonChildEnd();

    // Right after the transforms' restore at the end of the delivery (transforms::ViewBlockEnd): puts
    // what the display computed from the matrices between ticks at the tick's, with the engine's
    // functions (on ctx's stack; r3 to r5 are given back). Objects and bones the lifetime reports name
    // are left (transforms::Freed).
    void Settle(PPCContext& ctx, uint8_t* base);

    // Totals since the start, for the clock's report.
    struct Totals
    {
        uint64_t blocks = 0;            // view blocks settled
        uint64_t globals = 0;           // hierarchy globals computed again
        uint64_t locals = 0;            // hierarchy locals computed again
        uint64_t children = 0;          // skeleton walks run again
        uint64_t globalsChanged = 0, localsChanged = 0, childrenChanged = 0; // ... whose matrix changed
        uint64_t floatsChanged = 0;     // floats the settling changed
        uint64_t freed = 0;             // notes left: the object was freed or destroyed meanwhile
        uint64_t unknown = 0;           // JADE_LOCAL_FROM_GLOBAL calls of the display not classified (left)
        double maxShift = 0;            // the largest translation the settling moved, since the start
        uint32_t unknownCallers[4] = {}; // the first distinct callers of those calls

        // The counts since start (maxShift and the callers stay this one's).
        Totals operator-(const Totals& start) const;
    };
    Totals Current();
}
