#pragma once

// The pose between ticks (state interpolation plan, section 8.7; step 5 document of the plan's
// folder).
//
// The display draws a skinned object through the pose copy (JADE_POSE_COPY, 82580968), which hands
// each bone's matrix relative to the object to the frame's packet. The copy reads the object's
// additional matrices (Resolve, pose.cpp): with pointer entries, the global matrix of the bone's
// object ([bone+20]) made relative to [object+20], or the matrix an entry points at, already relative
// to the object; without, the object's own 68-byte matrices.
//
// The characters are two kinds of object (step 5 document, section 1): the animated object, which the
// display mounts and whose 68-byte additional matrices are the skeleton's bones relative to it, and
// its skins, which it renders itself (826B9860) with [skin+20] set to its own matrix for the call. A
// skin's pointer entries name the animated object and point at its bones. So the skeleton is the
// animated object's: its bones are captured and decided with its matrix, and written at each copy of
// one of its skins (or of the object itself). With step 4 alone the skins followed the object's
// blended matrix while the bones stayed at the tick (step 4 document, section 6).
//
// A skeleton belongs to its object's track in the transforms (transforms.cpp): it is captured at the
// same ticks and decided with it, so a pose is never made of two times (plan invariant 5):
// - a cut of the object holds the object and every bone at the previous tick;
// - a bone that jumps or turns in the object's own space (so the object's motion does not count
//   against each bone; an animation's snap) holds every bone at the previous tick. Bones relative to
//   the object (the characters') follow the object's own decision, so a snap of the pose does not
//   stop the object; with a global bone the whole actor holds (the copy makes it relative to the
//   object's matrix as drawn);
// - a skeleton that cannot continue (another layout, a bone not finite) leaves the whole actor at the
//   tick for the interval.
// The bones are written right before the copy and put back right after it (the reference's point of
// action), where the stored floats are still there. A bone the display changed since the tick, or
// that step 4 wrote as an object of its own, is left as it is (counted). ROAN_POSE=0 turns the pose
// off (step 4 alone, for a comparison).
//
// Main thread only: the calls come from the transforms and from engine_hooks.cpp inside the main
// step's display.

#include "game/interpolation/matrices.h"

#include <array>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace game::interpolation::pose
{
    bool Enabled();

    struct Bone
    {
        uint32_t matrix = 0;  // the 16 floats the copy reads
        uint32_t object = 0;  // the object the entry names (pointer entries), 0 otherwise
        bool global = false;  // [object+20] of the bone's object: made relative to the owner by the copy
        matrices::Bits previous{}, current{};
        // The step in the owner's space and the largest row turn (radians) of the interval before;
        // -1 unknown.
        float lastStep = -1.0f, lastAngle = -1.0f;
        uint64_t walked = 0; // the skeleton's capture (Skeleton::captures) at which Walked last set current
    };

    // The skeleton of one object, as of its last capture.
    struct Skeleton
    {
        uint32_t array = 0;     // the additional matrices' entries
        uint32_t count = 0;
        bool pointers = false;  // 12-byte pointer entries (identity flag 0x01000000)
        bool continuing = false; // every bone's previous holds the tick before
        uint64_t captures = 0;   // tick captures of this skeleton (its first included)
        std::vector<Bone> bones;
    };

    // What a skeleton's capture decided for its actor.
    enum class Decision
    {
        kContinue, // interpolate with the object (or nothing to decide: no skeleton)
        kHoldPose, // a bone jumps or turns, every bone relative to the object: the pose shows the
                   // previous tick, the object as decided for itself
        kCut,      // a bone jumps or turns, some bone global: the actor shows the previous tick
        kTick,     // the skeleton cannot be blended in this interval: the actor stays at the tick
    };

    // A capture of the object's skeleton at its tick. ownerPrevious and ownerCurrent are the object's
    // matrix at the two ticks (the owner's space of the cut test); ownerContinuing says whether
    // ownerPrevious holds the tick before; written are the matrices the transforms stored over in this
    // view block (a skeleton reading one is not captured). Marks the pages of a new skeleton for the
    // lifetime reports.
    Decision Capture(Skeleton& skeleton, uint32_t object, const matrices::Bits& ownerPrevious,
        const matrices::Bits& ownerCurrent, bool ownerContinuing, const std::unordered_set<uint32_t>& written);

    // The display's skeleton walk (826B96D8) wrote bone matrix of the skeleton from child's local only,
    // in the view block of the skeleton's capture. A bone of an object attached under another bone (a
    // headband's knot on an NPC's head) is written only by the walk, from the attached object's local and
    // its father's bone (or its local alone under the animated object), never by the logic: the capture
    // at the view block's start reads the value the last walk left, one tick old. Those inputs are the
    // tick's (the bones are written only around each copy), so the walk's output is the bone's value at
    // this tick: it becomes the bone's current. Between ticks the walk computes it again from the same
    // inputs, so the copy finds it as captured and blends it with the rest of the skeleton. The walk's
    // other paths (status bit 31: the local from the global; no hierarchy: from [animated object+20]) read
    // matrices the transforms wrote, and are left (the copy then leaves the bone, counted). A bone the
    // walk did not set at the previous capture (the object not drawn in that view block) holds for this
    // interval. Its step is not in the capture's cut test (the test runs before the walk).
    void Walked(Skeleton& skeleton, uint32_t child, uint32_t matrix);

    // Whether a lifetime report touches the skeleton (touches: the reported ranges' test).
    bool Touched(const Skeleton& skeleton, bool (*touches)(uint32_t begin, uint32_t end));

    // What the actor shows in this view block.
    enum class Show
    {
        kTick,     // nothing written
        kPrevious, // a cut
        kBlend,    // at alpha
    };

    // A tracked object's skeleton and what its actor shows; no skeleton when it is not tracked.
    struct Owner
    {
        const Skeleton* skeleton = nullptr;
        Show show = Show::kTick;
    };
    using FindOwner = Owner (*)(uint32_t object);

    // Right before the pose copy of object: stores each bone the copy will read for what the bone's
    // owner shows. The owner is the object an entry names when the entry points at that object's
    // captured 68-byte bones (a skin of an animated object), else the object itself when its skeleton
    // was captured. objectMatrices are the matrices step 4 wrote in this view block (left as written).
    void CopyStart(uint32_t object, float alpha, FindOwner find, const std::unordered_set<uint32_t>& objectMatrices);

    // Right after the copy: puts the tick's floats back where the stored ones are still there.
    void CopyEnd();

    // A new view block (BlockStart) and the skeleton cuts found at its tick capture (NoteCut).
    void BlockStart();
    void NoteCut();

    // The last view block, for the motion record.
    struct Block
    {
        uint32_t blended = 0;   // pose copies written at alpha
        uint32_t held = 0;      // ... at the previous tick (cut)
        uint32_t bones = 0;     // bone matrices written
        uint32_t partial = 0;   // copies with bones left (changed by the display, moved, step 4's)
        uint32_t cuts = 0;      // skeleton cuts found at this view block's tick capture
        uint32_t untracked = 0; // copies with bones of no captured skeleton
    };
    const Block& LastBlock();

    // Totals since the start, for the clock's report.
    struct Totals
    {
        uint64_t captures = 0;    // skeleton captures that continue
        uint64_t starts = 0;      // skeletons started (first capture, another layout)
        uint64_t cutJump = 0, cutTurn = 0;      // skeleton cuts by a bone, in the owner's space
        uint64_t poseHeld = 0;                  // ... that held only the pose (bones relative to the object)
        uint64_t tickLayout = 0, tickInvalid = 0; // actors left at the tick: another layout, a bone not finite
        uint64_t blended = 0, held = 0;         // pose copies written
        uint64_t bones = 0;                     // bone matrices written
        uint64_t untracked = 0;                 // copies with bones of no captured skeleton
        uint64_t borrowed = 0;                  // copies whose bones are another object's (a skin's)
        uint64_t walked = 0;      // bones the display's skeleton walk computed at the capture (Walked)
        uint64_t display = 0;     // bones the display changed since the tick (left)
        uint64_t moved = 0;       // bones whose entry names another matrix at the copy (left)
        uint64_t shared = 0;      // bones step 4 wrote as an object's matrix (left)
        uint64_t unrestored = 0;  // stored floats something else changed before the restore
        // Per continuing skeleton and tick, the largest bone step in the owner's space: below 0.05,
        // 0.1, 0.25, 0.5, 1 and 2 units, and above.
        std::array<uint64_t, 7> steps{};
        // ... and the largest bone turn: below 10, 20, 30, 45, 60 and 90 degrees, and above.
        std::array<uint64_t, 7> turns{};
        double maxStep = 0, maxTurn = 0; // since the start (degrees for the turn)

        // The counts since start (the maxima stay this one's).
        Totals operator-(const Totals& start) const;
    };
    Totals Current();
}
