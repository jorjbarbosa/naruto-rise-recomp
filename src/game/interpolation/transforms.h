#pragma once

// Objects and camera between ticks (state interpolation plan, section 8.6; step 4 document of the
// plan's folder).
//
// Under the gate the view block runs every frame and the logic once per tick. Each view block draws
// the objects and the camera between the last two ticks, at the view clock's image position (alpha,
// view_clock::Times): the state the frame shows is the previous tick's plus alpha of the way to the
// current one, the same time the display's phases read (step 3). A discontinuity between the two ticks
// (a cut) shows the previous tick's state for the whole interval, so the change appears at the next
// tick, in step with everything else.
//
// Objects. The set is what the display renders (825E28D0). An object is captured the first time it is
// rendered (at the state of its tick) and at every tick while the display keeps rendering it, so it
// interpolates from its second tick. Before each view block the blend is stored over its global
// matrix ([object+20]), which every consumer of the display reads (culling, lights, the packet); after
// the delivery the tick's floats go back where the stored ones are still there. A float the display
// writes (changed since the tick, or over the stored one: the look-at turn of modifier 5, the matrix
// modifiers 42 and 45 integrate in) is left as the display wrote it and is not written again for that
// object: the display animates it per call, by the view clock's dt. The type word is never written: a
// matrix whose scale bit changed between the ticks (the look-at icons at scale 1) is blended in the
// live type's convention (matrices::InTypeOf).
//
// Camera. The view entry's matrix and field of view, written at the display's first camera call for
// the entry (JADE_DISPLAY_CAMERA), put back after the delivery. A change of the view's father or
// viewport is a cut, and so is a step past the camera cut detector's thresholds (10 units, 30 degrees
// in a tick) that is also a jump against the step before (transforms.cpp).
//
// Cuts of objects: another matrix, a row turning more than 60 degrees in a tick, or a position step that
// is a jump against the object's own recent motion (transforms.cpp).
//
// Skeletons. A skinned object's bones belong to its track and are decided with it (pose.h): a bone's
// jump cuts the whole actor, a skeleton that cannot be blended leaves the actor at the tick, and the
// bones are written around the object's pose copy only.
//
// The tracked objects and matrices are dropped when lifetime.h reports them freed or destroyed. Off
// (no view block under the gate, or ROAN_TRANSFORMS=0 for a comparison with the gate alone) nothing is
// written and nothing is tracked. Main thread only: the calls come from engine_hooks.cpp, inside the
// main step.

#include <cstdint>

namespace game::interpolation::transforms
{
    // The view setup of a main step's view block, after the view clock's: captures the tick when the
    // logic ran since the last view block, then writes the objects' blends.
    void ViewBlockStart();

    // The end of the delivery, and of the main step: puts the tick's floats back. Nothing to do when
    // ViewBlockStart did not write.
    void ViewBlockEnd();

    // Whether the lifetime reports the last ViewBlockStart or ViewBlockEnd took touch [begin, end), on
    // pages watched before the frees (lifetime::Watch): what the display named in this view block may
    // have been destroyed under the display or by the delivery. True for anything when reports were
    // lost.
    bool Freed(uint32_t begin, uint32_t end);

    // The display's camera from a view entry (JADE_DISPLAY_CAMERA, r3), inside the main step's display.
    void Camera(uint32_t entry);

    // The display renders an object (JADE_RENDER_OBJECT, r6), inside the main step's display.
    void NoteRendered(uint32_t object);

    // Around the pose copy of an object (JADE_POSE_COPY, r3), inside the main step's display: its
    // skeleton's bones for what the object shows in this view block (pose.h), put back at the end.
    void PoseCopyStart(uint32_t object);
    void PoseCopyEnd();

    // After the display's skeleton walk (JADE_SKELETON_CHILD, r3 the child, r4 the animated object, r5
    // its bone), inside the main step's display: in the first view block after a tick, the bone the walk
    // computed from the child's local is the skeleton's at this tick (pose::Walked).
    void SkeletonChildWalked(uint32_t child, uint32_t object, uint32_t bone);

    // What Suspend holds at the tick: nothing, the objects (and their skeletons; the camera is still
    // written), or everything (the gate alone, as with ROAN_TRANSFORMS=0).
    enum class Suspended
    {
        kNone,
        kObjects,
        kAll,
    };

    // While suspended the view blocks under the gate do not write what is held, and its tracks are
    // dropped; the next resumed tick captures again. For the carry probe's comparisons in one run
    // (purity_probe.h).
    void Suspend(Suspended what);

    // The purity probe's replay of the view block (after the main step, with the originals): stores
    // again the floats the replayed view block was given, where the tick's are still there, so the
    // replay draws what that view block drew; Restore (or the end of the scope) puts the tick's back
    // where the stored ones are still there, so the probe compares only what the replay itself wrote.
    // Does nothing when that view block wrote nothing.
    class ReplayScope
    {
    public:
        ReplayScope();
        ~ReplayScope();
        ReplayScope(const ReplayScope&) = delete;
        ReplayScope& operator=(const ReplayScope&) = delete;

        void Restore();

    private:
        bool restored_ = false;
    };

    // The last view block, for the motion record.
    struct Block
    {
        bool active = false;
        double alpha = 0;          // the image position written at
        uint32_t written = 0;      // object matrices written
        uint32_t held = 0;         // ... at the previous tick (cut)
        uint32_t cuts = 0;         // object cuts found at this view block's tick capture
        uint32_t camera = 0;       // 0 not written, 1 blended, 2 held at the previous tick (cut)
        uint32_t cameraCut = 0;    // the cut's reasons (CameraCut bits of transforms.cpp), at the tick
    };
    const Block& LastBlock();

    // Totals since the start, for the clock's report.
    struct Totals
    {
        uint64_t blocks = 0;        // view blocks that wrote
        uint64_t starts = 0;        // object tracks started (first render, another matrix)
        uint64_t captures = 0;      // tick captures that continue a track
        uint64_t written = 0;       // object matrices written
        uint64_t held = 0;          // ... at the previous tick (cut)
        uint64_t partial = 0;       // ... with floats the display changed since the tick, left alone
        uint64_t changed = 0;       // restores that found floats the display wrote over ours (left)
        uint64_t changedTranslation = 0; // ... among them its translation: the logic reads a position the display made
        uint64_t cutJump = 0, cutTurn = 0, cutMatrix = 0, cutInvalid = 0;
        uint64_t scaleChanges = 0;  // continuing tracks whose scale bit changed (blended: matrices::InTypeOf)
        uint64_t fastKept = 0;      // steps above the reference's 4 units that the jump test interpolated
        uint64_t dropped = 0;       // tracks dropped: their object or matrix freed or destroyed
        uint64_t unrestored = 0;    // written floats freed before the restore (not put back)
        uint64_t cameraWritten = 0, cameraHeld = 0, cameraChanged = 0;
        uint64_t cameraFastKept = 0; // camera steps past the detector's thresholds the jump test interpolated
        uint64_t cameraCutMove = 0, cameraCutFather = 0, cameraCutViewport = 0, cameraCutInvalid = 0;
        double maxStep = 0;         // the largest position step of a continuing track, units per tick

        // The counts since start (maxStep stays this one's: the largest since the process started).
        Totals operator-(const Totals& start) const;
    };
    Totals Current();
}
