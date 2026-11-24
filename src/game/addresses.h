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
#define JADE_DISPLAY_CAMERA sub_82659B88 // the display's camera from a view entry (r3 entry): first per entry, again at the display's end
#define JADE_RENDER_OBJECT sub_825E28D0 // WOR_Render_One_GO: apply modifiers, render, unapply (r6 object)
#define JADE_BONE_COPY sub_820E91E0     // copies a bone matrix into the frame's packet (r3 bone, r4 matrix)
#define JADE_POSE_COPY sub_82580968     // GEO_SKN_Compute4Display: the skin's bones, relative to the object, to JADE_BONE_COPY (r3 object, r4 geometry)
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

// Hierarchy and skeleton matrices (step 6 document, section 1). The display calls them from the Apply
// of modifiers 5, 14, 22, 26 and 45 and from the skeleton walk of an animated object's render.
#define JADE_GLOBAL_FROM_HIERARCHY sub_826A52E8 // r4 = the locals of r3 and its fathers up to one without a father, times that one's global (r5 1)
#define JADE_LOCAL_FROM_GLOBAL sub_826A5998     // a hierarchy object's local from its global and its father's (r3 object)
#define JADE_SKELETON_CHILD sub_826B96D8        // a skeleton object (r3) of an animated object (r4): its bone (r5) and global, or its local first

// Readers of the game time (step 3 document, section 4). On the display's path, main thread:
#define JADE_MODIFIER_1_APPLY sub_8260B188   // OnduleTonCorps: amplitudes and a fade from the game time
#define JADE_MODIFIER_26_APPLY sub_826089D8  // the object's scale from a sine of the game time
#define JADE_MODIFIER_37_STEP sub_82608138   // type 37's Apply step: sines and wraps of the game time (r3 record)
#define JADE_MODIFIER_42_ANGLE sub_825D2820  // types 42 and 45: an angle, from the game time when f1 <= 0
#define JADE_MODIFIER_47_TIME sub_82635778   // type 47: hands the game time to a shader parameter
#define JADE_MODIFIER_59_APPLY sub_8269EEC8  // vegetation wind: a phase from the game time, then the wind field
#define JADE_WIND_FIELD sub_8267D450         // steps to the next wind source once per distinct game time
#define JADE_GRASS_WAVES sub_825972A8        // the grass' nine wave cosines of the game time
#define JADE_GRASS_GUST sub_82597780         // the grass' gust from the game time, into the draw block
#define JADE_GRASS_SCROLL sub_82599CD8       // adds a step from the game time to the grass scroll (r3 +312), per call
#define JADE_TECHNIQUE_WAVE sub_82103770     // a technique's texture wave from the game time and the position
// On the render thread (screen effects, vtable slot +20 of their classes):
#define JADE_SCREEN_EFFECT_FADE sub_8211BF38
#define JADE_SCREEN_EFFECT_SCROLL sub_8211C278
#define JADE_SCREEN_EFFECT_PROGRESS sub_8211EA08

// The save thumbnail (save_thumbnail.h). The request (820ECD38, from the save's start 82651F88) sets
// the capture task's +60 to the thumbnail and its +8 (pending); at every packet hand-off 820F01D8 sets
// each task's +4 (enabled) from +8, then calls its vtable +44, for the capture task 8211D7B0
// (+56 = +60); the render thread (820F2470) runs vtable +20, the capture 8211D878, when +4 is set.
#define JADE_CAPTURE_TASK_COPY sub_8211D7B0 // the capture task's per-hand-off copy (r3 the task): +56 = +60
#define JADE_THUMBNAIL_CAPTURE sub_8211D878 // render thread: draws, resolves, saves and compresses into [task+56]; clears +8 and +60; 1 on success
#define JADE_THUMBNAIL_RELEASE sub_82583D28 // frees a thumbnail's two buffers (r3 the thumbnail); every free of one follows it

// The shape of the picture (widescreen.cpp). The interface manager ([kEffects]) draws its groups in
// JADE_EFFECTS_DRAW; each draw places its item with JADE_INTERFACE_PLACE first.
#define JADE_CAMERA_ASSIGN sub_82584860       // CAM_AssignCameraToDevice: a camera's factors and size for the device (r3 screen format, r4 camera, r5 width, r6 height)
#define JADE_INTERFACE_PLACE sub_820C3A58     // r3 = the placement at r4 (none: the whole screen) inside the one at r5 (none: the whole screen)
#define JADE_INTERFACE_MOVIE_DRAW sub_826A42B8 // draws a Flash movie item (r3 item, r4 the placement it is in)
#define JADE_INTERFACE_IMAGE_DRAW sub_8264F1A0 // draws an item that is one quad (r3 item, r4 the placement it is in)
#define JADE_INTERFACE_TEXT_DRAW sub_8264F768  // draws a text item (r3 item, r4 the placement it is in)
#define JADE_MOVIE_DISPLAY sub_826B03E0        // the engine's movie wrapper displays its movie (r3 wrapper): places it, then the view's Display
#define JADE_VIDEO_QUAD sub_820EC008           // a Bink video item's frame as a quad of the frame's size in device pixels (r3 video)
#define JADE_VERTEX_ALLOCATE sub_82101C00      // r3 = room for a geometry's vertices in the frame's vertex memory (r3 geometry, r4 bytes, r5 stride)

// Scaleform GFx 2.x, linked into default.xex (interface_resolution.cpp;
// docs/development/plans/image-quality/e7-interface-resolution.md). The game never sets a viewport:
// GFx's SetLevelMovie (821E05E0) sets each movie's to its stage size when the movie gets its level 0.
#define GFX_SET_VIEWPORT sub_821E13B0  // GFxMovieRoot::SetViewport (r3 movie, r4 the GViewport), vtable +80
#define GFX_MOVIE_DESTROY sub_821E2E70 // ~GFxMovieRoot (r3 movie), from the deleting destructor at vtable +0

// The game's GFx renderer (vtable 82085798, built by 825E8778; widescreen_shapes.cpp,
// docs/development/plans/wide-interface/study.md): GFx draws every movie through it, shape by shape.
#define GFX_RENDERER_SET_MATRIX sub_825E8490   // SetMatrix (r3 renderer, r4 the shape's 2x3 matrix to the stage, in twips), vtable +20
#define GFX_RENDERER_SET_VERTICES sub_825E9130 // SetVertexData (r3 renderer, r4 vertices, r5 count, r6 format), vtable +40
#define GFX_RENDERER_DRAW_SHAPE sub_825E8060   // DrawIndexedTriList (r3 renderer, r4 base vertex, r5 first, r6 count, r7 first index, r8 triangles), vtable +52

// The renderer's draw records: every draw the engine asks for fills one in the packet being built and
// submits it; the render thread makes the device's draw of each record of the packet it was handed.
#define JADE_RECORD_SUBMIT sub_820E7D40        // adds the packet's current record to its pass (r3 packet)
#define JADE_RECORD_DRAW sub_821024A8          // the device's draw of a record (r3 packet, r4 record): the only caller of the device's draws

// The shader manager ([82C0F03C], shader_lookup.cpp): every material setup asks it for a vertex
// and a pixel shader by key. A miss creates the shader from the bank and inserts it (820F5C58 vertex,
// 820F5D38 pixel); the loading queue (820F94C0, through 820F5E18/820F5EE8) and the setup's 821221A8
// insert through the same two, and an insert on a present key replaces the node's shader in place.
// Only the destruction erases nodes (820F5828 and the tree erase 820F6970 have no other caller).
#define JADE_GET_VERTEX_SHADER sub_820F5A80        // r3 = the shader of the key object r4, from the manager r3's vertex map
#define JADE_GET_PIXEL_SHADER sub_820F5B68         // the same from its pixel map
#define JADE_SHADER_MANAGER_DESTROY sub_820F56C8   // frees the manager's shaders and the nodes of both maps (r3 manager; from 820E7208)

// The menus (menu_entries.h; docs/development/plans/port-menus/game-side-findings.md sections 2 to 4).
// Natives of default.xex that AI2C.dll calls through its native table, and one track of AI2C.dll.
#define AI_OPTIONS_TRACK sub_8810B248     // fct_9E002BAB: the Options screen's track, every tick of state 27 (returns 1000)
#define AI_BUTTON_JUST_PRESSED sub_825A3448 // native 7006 IO_ButtonJustPressed (r3 player, r4 button; r3 1 when pressed)
#define AI_EXCLUSIVE_CONTENT sub_825AE948 // native 16455: XLaunchNewImage("LinkApp.xex"), the main menu's 5th entry
#define AI_MNU_VAR_SET_TEXT sub_82693880  // native 21268 MNU_VarSetText (r3 movie, r4 variable name, r5 text id)
#define AI_MNU_VAR_SET_STRING sub_82693950 // native 21267 MNU_VarSetString (r3 movie, r4 variable name, r5 string)
// The game's option store (game_options.h; game-side-findings.md section 6). Each takes the store's
// instance first and saves and restores the AI context itself.
#define AI_OPTIONS_LOAD sub_880629D0 // load from the profile (r3 store), then validate and apply all
#define AI_OPTION_GET sub_88064500   // fni_Option_GetValue (r3 store, r4 int* out, r5 index)
#define AI_OPTION_SET sub_88061C20   // fn_Option_SetValue (r3 store, r4 int* changed, r5 index, r6 value): clamps, applies
#define AI_OPTIONS_SAVE sub_88062B30 // save to the profile (r3 store, r4 int* ok)
#define AI_OPTION_APPLY sub_88061FB0 // fn_Option_Apply (r3 store, r4 index): applies the stored value

namespace game::jade
{
    // Globals.
    constexpr uint32_t kTime = 0x82C0F080;        // pointer to the time structure (tim::)
    constexpr uint32_t kWorld = 0x82709C3C;       // the world of the view block
    constexpr uint32_t kView = 0x82709C40;        // the view object (view::), also the display data
    constexpr uint32_t kViewsOff = 0x82C0EBB4;    // nonzero: the main step skips the view block
    constexpr uint32_t kEffects = 0x82C0EB04;     // the world's effects (first argument of JADE_EFFECTS_DRAW)
    constexpr uint32_t kCutCounter = 0x82C0EB34;  // camera cut counter: 4 on a cut, minus one per display
    // The camera cut detector (8267A2A0, called by the display) also keeps the last camera it saw in
    // the scene manager's block (words 1 to 52: the view matrix, a view-projection cache, the first view
    // entry's position, the view direction), and sets the fade rate and a flag from the sector manager.
    constexpr uint32_t kCutDetector = 0x82C0F1F8;
    constexpr uint32_t kCutDetectorState = 4;      // offset of its state in the block
    constexpr uint32_t kCutDetectorStateSize = 208;
    constexpr uint32_t kCutFadeRate = 0x826F6368;  // float: what the fade helper 82679C68 moves by, times the dt
    constexpr uint32_t kCutFlag = 0x82C0EB30;
    constexpr uint32_t kDisplayData = 0x82C0EFA4; // the view being displayed
    constexpr uint32_t kPacketPending = 0x82C0F038; // set by JADE_HAND_PACKET: 1 when the handed packet is to be drawn
    constexpr uint32_t kDeviceLock = 0x82C0F008;    // pointer to the D3D device lock (device_lock::)
    constexpr uint32_t kDeferredQueue = 0x82C0EBF4; // pointer to JADE_DEFERRED_DESTROY's queue: +4 its count
    constexpr uint32_t kDisplayDestroyCount = 0x82C0EBE0; // objects JADE_DISPLAY_DESTROYS will destroy
    constexpr uint32_t kSectors = 0x8272F2B0;       // the sector manager: JADE_SECTOR_SWITCH works only when it and its +44 are set
    constexpr uint32_t kSectorsTarget = 44;
    // The engine's input flag: while any bit is set, every script pad read and the fight's pad logic
    // see nothing (82551C78 and its siblings). The game uses bits 4 (a movie waiting for a button), 8
    // (the scripts, natives 7033/7034) and 0x10 (its message box); the word is zeroed once at start
    // (82684128, from main). docs/development/plans/port-menus/game-side-findings.md section 5.
    constexpr uint32_t kInputFlag = 0x82C0F0D8;
    // byte_82C0EE41: 1 while the game has taken an XN_SYS_UI 1 (its pause: time, sounds, interface),
    // 0 after the 0. The notification handler (825F7D90) runs in the main loop, not during a Bink movie.
    constexpr uint32_t kSystemUiTaken = 0x82C0EE41;

    // The D3D device lock ([kDeviceLock]), recursive per thread.
    namespace device_lock
    {
        constexpr uint32_t kOwner = 4;   // the owner's thread id, -1 when free
        constexpr uint32_t kCount = 8;   // recursion count
        constexpr uint32_t kDevice = 16; // the D3D device the record draws use (821024A8)
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
        // 64-bit, µs: the game time spent locked (+92 nonzero); the phase consumers read the game time
        // minus it, some through its low word (+100).
        constexpr uint32_t kLocked = 96;
        constexpr uint32_t kSize = 136;
    }

    // The grass patch handed to JADE_GRASS_SCROLL.
    namespace grass
    {
        constexpr uint32_t kScroll = 312; // float: the scroll, plus one step per call
    }

    // The view object ([kView]).
    namespace view
    {
        constexpr uint32_t kCurrentObject = 228;   // the object being rendered or applied
        constexpr uint32_t kFieldOfView = 300;     // float, from the view entry (JADE_DISPLAY_CAMERA)
        constexpr uint32_t kCameraDirection = 460; // 3 floats
        constexpr uint32_t kCameraPosition = 476;  // 3 floats
        constexpr uint32_t kWorld = 2248;          // the world the view block displays
        // ul_RenderingCounter: +1 per view block in GDI_BeforeDisplay (8259E908, JADE_VIEW_PREPARE)
        // unless the display flags (+2240) have DoNotRender (0x80).
        constexpr uint32_t kRenderingCounter = 0x964;
        // The extent the probes label: up to the last field the delivery writes (+2572, 8259FF40).
        constexpr uint32_t kProbeSize = 2576;
    }

    // A camera (CAM_tdst_Camera; the view's is at view +288), as JADE_CAMERA_ASSIGN leaves it.
    namespace camera
    {
        constexpr uint32_t kFlags = 0;
        constexpr uint32_t kFlagPerspective = 1; // otherwise orthographic, with a square screen ratio
        constexpr uint32_t kFactorX = 20;        // float: half the camera's width times the screen ratio, over the tangent of half the field of view
        constexpr uint32_t kScreenRatio = 76;    // float: the screen's height over its width
    }

    // The interface manager ([kEffects]).
    namespace interface_manager
    {
        constexpr uint32_t kGroups = 8;      // array of group addresses
        constexpr uint32_t kGroupCount = 12;
    }

    // An item of the interface manager: a group, or a movie, image, text or video in one.
    namespace interface_item
    {
        constexpr uint32_t kVtable = 0;
        constexpr uint32_t kMovieVtable = 0x82084958; // of a Flash movie item
        constexpr uint32_t kFlags = 4;
        constexpr uint32_t kShown = 0xE0000000; // in kFlags: created, open and shown, what every draw tests first
        constexpr uint32_t kGroupItems = 92;    // a group's array of item addresses
        constexpr uint32_t kGroupItemCount = 96;
        constexpr uint32_t kMovieName = 100; // a movie item's name: a string object, the characters' address first
        constexpr uint32_t kNameFlags = 4;   // of the string object
        constexpr uint32_t kNameWide = 8;    // in kNameFlags: 16-bit characters
        constexpr uint32_t kMovie = 124;     // a movie item's movie, 0 until it is open
    }

    // The engine's movie wrapper (a movie item's kMovie).
    namespace movie
    {
        constexpr uint32_t kView = 96; // its GFxMovieRoot, 0 until it is created
    }

    // GFx's GViewport, as SetViewport takes it: 13 words.
    namespace gfx_viewport
    {
        constexpr uint32_t kBufferWidth = 0; // integers, pixels, as the rest up to kScale
        constexpr uint32_t kBufferHeight = 4;
        constexpr uint32_t kLeft = 8;
        constexpr uint32_t kTop = 12;
        constexpr uint32_t kWidth = 16;
        constexpr uint32_t kHeight = 20;
        constexpr uint32_t kScissorLeft = 24;
        constexpr uint32_t kScissorTop = 28;
        constexpr uint32_t kScissorWidth = 32;
        constexpr uint32_t kScissorHeight = 36;
        constexpr uint32_t kScale = 40;       // float
        constexpr uint32_t kAspectRatio = 44; // float
        constexpr uint32_t kFlags = 48;
        constexpr uint32_t kSize = 52;
    }

    // The game's GFx renderer: the fill its DrawIndexedTriList draws with, and the vertices GFx hands it.
    namespace gfx_renderer
    {
        constexpr uint32_t kFillMode = 436; // 1 a color, 2 a bitmap, 3 gouraud (with kTexture: a bitmap faded by the vertices)
        constexpr uint32_t kFillBitmap = 2;
        constexpr uint32_t kFillGouraud = 3;
        constexpr uint32_t kTexture = 544;  // the bitmap's texture; 0 for a gouraud fill without one
        // A bitmap's texture coordinates from the shape's: u = a x + b y + c over the texture's width,
        // v likewise over its height (floats; FillStyleBitmap, 825E8398).
        constexpr uint32_t kTextureU[3] = { 492, 496, 504 };
        constexpr uint32_t kTextureV[3] = { 508, 512, 520 };
        constexpr uint32_t kWrap = 548; // 0 the bitmap repeats, 2 it is clamped
        constexpr uint32_t kWrapRepeat = 0;
        // SetVertexData's formats: x and y as 16-bit integers (twips), then a color, or a color and a factor.
        constexpr uint32_t kVertexXY = 1;
        constexpr uint32_t kVertexXYColor = 3;
        constexpr uint32_t kVertexXYColorFactor = 4;
    }

    // A placement of the interface: 4 floats, left, top, width and height as shares of the screen.
    namespace placement
    {
        constexpr uint32_t kLeft = 0;
        constexpr uint32_t kTop = 4;
        constexpr uint32_t kWidth = 8;
        constexpr uint32_t kHeight = 12;
    }

    // The renderer ([kRenderer]): two packets, one being built while the render thread draws the other.
    constexpr uint32_t kRenderer = 0x82C0F020;
    namespace renderer
    {
        constexpr uint32_t kPacketSize = 3208;
        constexpr uint32_t kPacketCount = 2;
        constexpr uint32_t kBuiltPacket = 6416; // index of the packet being built; a hand-off flips it
    }

    // A packet of the renderer.
    namespace packet
    {
        constexpr uint32_t kCurrentRecord = 3060; // the record being filled, 0 once submitted; records of a packet are filled at rising addresses
        constexpr uint32_t kRecordSize = 224;
    }

    // The quad JADE_VIDEO_QUAD writes: 4 vertices of x, y, z, u, v in clip space.
    namespace video_quad
    {
        constexpr uint32_t kVertices = 4;
        constexpr uint32_t kStride = 20;
    }

    // A view entry (WOR_tdst_View, 172 bytes; the world's +48 holds them): what the display builds its
    // camera from (JADE_DISPLAY_CAMERA).
    namespace view_entry
    {
        constexpr uint32_t kMatrix = 0;         // st_ViewPoint: 16 floats and a type word
        constexpr uint32_t kFather = 136;       // pst_Father, the object the view follows (native 12500)
        constexpr uint32_t kFieldOfView = 148;  // float (native 12505)
        constexpr uint32_t kViewport = 152;     // 4 floats: left, top, width, height
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

        constexpr uint32_t kIdentityAdditionalMatrices = 0x00200000; // has additional matrices (the pose copy reads them)
        constexpr uint32_t kIdentityHierarchy = 0x00400000;          // has a hierarchy block (base::kHierarchy)
        constexpr uint32_t kIdentityAdditionalPointers = 0x01000000; // additional matrices are GizmoPtr entries
        constexpr uint32_t kStatusMounted = 0x00040000;              // the mount renders it...
        constexpr uint32_t kStatusNotMounted = 0x00200004;           // ...unless ForceInvisible or Culled
        // In a skeleton (JADE_SKELETON_CHILD): the walk computes the object's local from its global,
        // and its bone from that, instead of its global from the bone.
        constexpr uint32_t kStatusGlobalToBone = 0x80000000;
    }

    // OBJ_tdst_Base.
    namespace base
    {
        constexpr uint32_t kDynamics = 4;           // DYN_tdst_Dyna: first word bit 31 set when active
        constexpr uint32_t kHierarchy = 8;          // the hierarchy block (hierarchy::), with kIdentityHierarchy
        constexpr uint32_t kAdditionalMatrices = 12; // OBJ_tdst_AdditionalMatrix (additional::)
        constexpr uint32_t kSize = 20;
    }

    // The hierarchy block of an object ([[object+28]+8]).
    namespace hierarchy
    {
        constexpr uint32_t kFather = 4; // the father object, or 0
        constexpr uint32_t kLocal = 8;  // the local matrix: global = local x the father's global
    }

    // OBJ_tdst_AdditionalMatrix: the bones of a skinned object.
    namespace additional
    {
        constexpr uint32_t kCount = 0; // signed: the pose copy reads entry i only when i < count
        constexpr uint32_t kArray = 4; // 68-byte matrices, or gizmo:: entries with kIdentityAdditionalPointers
    }

    // OBJ_tdst_GizmoPtr, 12 bytes. The pose copy (JADE_POSE_COPY) takes the global matrix of kObject
    // ([kObject+20], made relative to the skinned object) when kId is kIdGlobal or kObject has
    // kIdentityAdditionalPointers, then the identity when kObject is null; otherwise kMatrix, already
    // relative to the skinned object.
    namespace gizmo
    {
        constexpr uint32_t kMatrix = 0;
        constexpr uint32_t kObject = 4;
        constexpr uint32_t kId = 8;
        constexpr uint32_t kIdGlobal = 0xFFFFFFFF;
        constexpr uint32_t kSize = 12;
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
    }

    // The renderer's thumbnail capture task (one object, renderer +168).
    namespace capture
    {
        constexpr uint32_t kVtable = 0x82087C28;
        constexpr uint32_t kEnabled = 4;    // read by the render thread's dispatch (vtable +40)
        constexpr uint32_t kPending = 8;    // set by the request, cleared by the capture
        constexpr uint32_t kThumbnail = 56; // the capture's thumbnail
        constexpr uint32_t kRequest = 60;   // the requested thumbnail, cleared by the capture
    }

    // The shader manager's maps: std::map from a 64-bit shader key to a shader object.
    namespace shader_manager
    {
        constexpr uint32_t kVertexMap = 0;
        constexpr uint32_t kPixelMap = 12;
    }

    // A std::map of the title's library (Dinkumware): the head node at +4. A node holds its left child,
    // parent and right child, the key at +16 (an unsigned 64-bit key in the shader maps) and the value
    // at +24 (the shader); the head and the leaves are marked by the byte at +33.
    namespace std_map
    {
        constexpr uint32_t kHead = 4;
        constexpr uint32_t kLeft = 0;
        constexpr uint32_t kParent = 4;
        constexpr uint32_t kRight = 8;
        constexpr uint32_t kKey = 16;
        constexpr uint32_t kValue = 24;
        constexpr uint32_t kIsNil = 33;
    }

    // A shader key object (the classes of vtables 82086930, vertex, and 82086980, pixel): the getters
    // read the key through its vtable +0, which in every key class returns the 64-bit key at +8.
    namespace shader_key
    {
        constexpr uint32_t kGetKey = 0x820F7300;
        constexpr uint32_t kKey = 8;
    }

    // A save thumbnail (20 bytes, allocated by the save's start 82651F88).
    namespace thumbnail
    {
        constexpr uint32_t kReady = 16; // 1 once captured (the capture's last store)
    }
}

// AI2C.dll's compiled scripts (the menu model, menu-screens.md).
namespace game::ai
{
    // A slot of AI2C.dll's data: points to the word that holds the running AI object
    // (sub_88397450). The object's +72 holds its model variables, words at byte offsets.
    constexpr uint32_t kCurrentObject = 0x883B2118;
    constexpr uint32_t kVariables = 72;
    // The native table's slot of the native the Options track calls with 1 before it shows the
    // Japanese voices' row (non-zero: offered); its result and g_200 decide the ninth row.
    constexpr uint32_t kJapaneseVoicesNative = 0x883B3304;

    // The menu model's variables.
    namespace menu
    {
        // g_2908: 1 on a state's first ticks, set by the state change (880EC2F8); the Options track
        // creates its screen while it is 1, then sets it to 0.
        constexpr uint32_t kStateEntering = 2908;
        // g_200: 1 when Options was entered from the main menu (it goes back there), else the story pause.
        constexpr uint32_t kFromMainMenu = 200;
        constexpr uint32_t kTrackIdle = 1000; // what a track returns to stay in its state
        constexpr uint32_t kExclusiveContentText = 21827; // the label of the main menu's 5th entry
    }
}
