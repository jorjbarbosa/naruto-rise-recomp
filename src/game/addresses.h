#pragma once

// Guest addresses of the Jade engine in default.xex that the game patches use, and the offsets of the
// engine structures they read, named in one place. Read in the binary unless a comment says
// otherwise; the state interpolation plan (docs/development/state-interpolation.md) has the context.

#include "ppc/ppc_hook.h"

#include <cstdint>

// Hooked functions: the symbols of the recompiled functions (see ppc/ppc_hook.h).
// The main loop (82684920) calls the first three, in this order, once per iteration; nothing else
// calls them.
#define JADE_HUD_MAP sub_82677D20       // the HUD map; its result is the world change's r3
#define JADE_WORLD_CHANGE sub_826843D8  // the pending world change (and the file cache's one-time setup)
#define JADE_MAIN_STEP sub_82684518     // one loop iteration: logic, view block, end of frame
#define JADE_VIEW_SETUP sub_82684050    // first call of the view block (r3 view)
#define JADE_VIEW_PREPARE sub_8259E908  // second call of the view block (r3 view)
#define JADE_DISPLAY sub_8265A058       // the display: camera, mount, renders (r3 world, r4 view)
#define JADE_EFFECTS_DRAW sub_826AC2E8  // waits for the effects' update and draws them (r3 effects, r4 view)
#define JADE_DELIVER sub_8259FF40       // hands the frame to the render thread (r3 view, r4 sector switch)
#define JADE_RENDER_OBJECT sub_825E28D0 // WOR_Render_One_GO: apply modifiers, render, unapply (r6 object)
#define JADE_BONE_COPY sub_820E91E0     // copies a bone matrix into the frame's packet (r3 bone, r4 matrix)
#define JADE_HAND_PACKET sub_820EFFB8   // main thread: hands the filled packet to the render thread
#define JADE_DRAW_PACKET sub_820FC008   // render thread: draws a handed packet (r3 packet)
#define JADE_DEVICE_LOCK sub_820E73A8   // takes the D3D device lock (recursive, per thread; r3 the lock)
#define JADE_DEVICE_UNLOCK sub_820E7428 // releases it
#define JADE_END_OF_FRAME sub_8267DDC0  // main step row 11: the sector switch if the delivery did not run it, deferred destruction
#define JADE_DISPLAY_DESTROYS sub_82659F40 // under the display: destroys the objects queued at kDisplayDestroyCount
#define JADE_SECTOR_SWITCH sub_8267DA90 // r3 the switch state (+4 set: ran this tick); destroys the sectors it leaves
#define JADE_DEFERRED_DESTROY sub_82659060 // r3 the queue (+4 count): releases, destroys and frees what it holds
#define JADE_DESTROY_OBJECT sub_8266BA98 // destroys a game object (r3)
#define JADE_DESTROY_OBJECT_CHECKED sub_8266C1F0 // drops the object (r3) from a world change list, then JADE_DESTROY_OBJECT
#define JADE_FREE sub_820C0018          // the engine's free: through the memory manager's vtable +20
#define JADE_HEAP_FREE sub_82188C88     // the heap's free (r3 heap, r4 block): every free and realloc ends here

namespace game::jade
{
    // Globals.
    constexpr uint32_t kTime = 0x82C0F080;        // pointer to the time structure (tim::)
    constexpr uint32_t kWorld = 0x82709C3C;       // the world of the view block
    constexpr uint32_t kView = 0x82709C40;        // the view object (view::), also the display data
    constexpr uint32_t kViewsOff = 0x82C0EBB4;    // nonzero: the main step skips the view block
    constexpr uint32_t kEffects = 0x82C0EB04;     // the world's effects (first argument of JADE_EFFECTS_DRAW)
    constexpr uint32_t kCutCounter = 0x82C0EB34;  // camera cut counter: 4 on a cut, minus one per display
    constexpr uint32_t kDisplayData = 0x82C0EFA4; // the view being displayed
    constexpr uint32_t kPacketPending = 0x82C0F038; // set by JADE_HAND_PACKET: 1 when the handed packet is to be drawn
    constexpr uint32_t kDeviceLock = 0x82C0F008;    // pointer to the D3D device lock (device_lock::)
    constexpr uint32_t kDeferredQueue = 0x82C0EBF4; // pointer to JADE_DEFERRED_DESTROY's queue: +4 its count
    constexpr uint32_t kDisplayDestroyCount = 0x82C0EBE0; // objects JADE_DISPLAY_DESTROYS will destroy
    constexpr uint32_t kSectors = 0x8272F2B0;       // the sector manager: JADE_SECTOR_SWITCH works only when it and its +44 are set
    constexpr uint32_t kSectorsTarget = 44;

    // The D3D device lock ([kDeviceLock]), recursive per thread.
    namespace device_lock
    {
        constexpr uint32_t kOwner = 4; // the owner's thread id, -1 when free
        constexpr uint32_t kCount = 8; // recursion count
    }

    // The time structure ([kTime]).
    namespace tim
    {
        constexpr uint32_t kGameTime = 48; // 64-bit, µs: the sum of the dt
        constexpr uint32_t kDt = 56;       // 64-bit, µs, with the time scale
        constexpr uint32_t kDtScaled = 64; // float, seconds, with the time scale
        constexpr uint32_t kDtRaw = 68;    // float, seconds
        constexpr uint32_t kFixedStep = 72; // nonzero: kDtRaw is kPeriod and the clock is not read
        constexpr uint32_t kPeriod = 76;    // float, seconds: the fixed step's period
        constexpr uint32_t kSize = 136;
    }

    // The view object ([kView]).
    namespace view
    {
        constexpr uint32_t kCurrentObject = 228;   // the object being rendered or applied
        constexpr uint32_t kCameraDirection = 460; // 3 floats
        constexpr uint32_t kCameraPosition = 476;  // 3 floats
        constexpr uint32_t kWorld = 2248;          // the world the view block displays
        // ul_RenderingCounter: +1 per view block in GDI_BeforeDisplay (8259E908, JADE_VIEW_PREPARE)
        // unless the display flags (+2240) have DoNotRender (0x80).
        constexpr uint32_t kRenderingCounter = 0x964;
        // The extent the probes label: up to the last field the delivery writes (+2572, 8259FF40).
        constexpr uint32_t kProbeSize = 2576;
    }

    // World: [+48] is its display data, whose +144 the main step points at the view.
    namespace world
    {
        constexpr uint32_t kDisplay = 48;
        constexpr uint32_t kDisplayView = 144;
    }

    // Game object (OBJ_tdst_GameObject of this branch).
    namespace object
    {
        constexpr uint32_t kIdentity = 0;
        constexpr uint32_t kStatus = 4;
        constexpr uint32_t kMatrix = 20;   // pst_GlobalMatrix
        constexpr uint32_t kBase = 28;     // pst_Base
        constexpr uint32_t kExtended = 32; // pst_Extended
        // The extent the probes label (the structure ends before it).
        constexpr uint32_t kProbeSize = 96;

        constexpr uint32_t kIdentityAdditionalPointers = 0x01000000; // additional matrices are GizmoPtr entries
        constexpr uint32_t kStatusMounted = 0x00040000;              // the mount renders it...
        constexpr uint32_t kStatusNotMounted = 0x00200004;           // ...unless ForceInvisible or Culled
    }

    // OBJ_tdst_Base.
    namespace base
    {
        constexpr uint32_t kDynamics = 4;           // DYN_tdst_Dyna: first word bit 31 set when active
        constexpr uint32_t kAdditionalMatrices = 12; // OBJ_tdst_AdditionalMatrix: +0 count, +4 array
        constexpr uint32_t kSize = 20;
    }

    // OBJ_tdst_Extended.
    namespace extended
    {
        constexpr uint32_t kModifiers = 32; // first modifier
        constexpr uint32_t kSize = 48;
    }

    // MDF_tdst_Modifier of this branch (one word more than the source's before the data).
    namespace modifier
    {
        constexpr uint32_t kInterface = 0; // its first word is the modifier type
        constexpr uint32_t kData = 12;
        constexpr uint32_t kNext = 16;
        constexpr uint32_t kSize = 32;
    }

    // Matrices: 16 floats (rows I, J, K, T with a fourth float each) and a type word.
    namespace matrix
    {
        constexpr uint32_t kTranslation = 48;
        constexpr uint32_t kType = 64;
        // Type bit 8 (MATH_Ci_Scale in the source): unit rows, the scale of each row in its fourth float.
        constexpr uint32_t kTypeScale = 8;
        constexpr uint32_t kSize = 68;
        constexpr uint32_t kGizmoPtrSize = 12; // OBJ_tdst_GizmoPtr: +0 the matrix
    }
}
