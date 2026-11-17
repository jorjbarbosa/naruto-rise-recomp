// The pose of skinned objects between ticks (pose.h).

#include "game/interpolation/pose.h"

#include "game/addresses.h"
#include "game/interpolation/diagnostics.h"
#include "game/interpolation/lifetime.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>

namespace game::interpolation::pose
{
    namespace
    {
        using matrices::Bits;
        using matrices::Float;
        using matrices::Vector;

        // Cuts by a bone, per tick, in the owner's space. A position step is a jump when it is above
        // kJumpFloor and kJumpRatio times the bone's step of the tick before; without one (a
        // skeleton's first interval) above kFirstJump. A row turning more than 60 degrees (the
        // reference's) is a cut when it is also kJumpRatio times the bone's turn of the tick before.
        // So a fast swing that builds up over ticks keeps interpolating and a snap of the animation
        // is still a cut (the camera's rule, transforms.cpp; objects cut on any turn past 60 degrees,
        // with the same jump test for their position). The cuts
        // measured in fights are such snaps (step 5 document, section 3): a hand closing, a body
        // turning 100 to 170 degrees in a tick as an attack starts; the row blend cannot follow a turn
        // past 90 degrees (it takes the short way, and two opposite rows cancel).
        constexpr float kFirstJump = 1.0f;
        constexpr float kJumpFloor = 0.5f;
        constexpr float kJumpRatio = 4.0f;
        constexpr float kTurnCosine = 0.5f;
        // A bound that keeps a damaged structure from turning into a huge walk.
        constexpr int32_t kMaxBones = 1024;

        using lifetime::kObjectExtent;
        constexpr float kStepBuckets[] = { 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f };
        constexpr float kTurnBuckets[] = { 10.0f, 20.0f, 30.0f, 45.0f, 60.0f, 90.0f };

        // A bone as the copy would read it now: the matrix, and the object the entry names (the bone's
        // object for a global matrix, else the object whose matrix it points at, or 0).
        struct Entry
        {
            uint32_t matrix = 0, object = 0;
            bool global = false;

            bool operator==(const Entry&) const = default;
        };

        struct Layout
        {
            uint32_t array = 0, count = 0;
            bool pointers = false;
        };

        enum class Found
        {
            kNone,    // the copy reads no bone
            kBones,
            kTooMany, // the copy reads bones this does not follow
        };

        // A float stored over a bone, put back after the copy.
        struct Stored
        {
            uint32_t matrix = 0;
            uint32_t mask = 0; // bit i: float i
            Bits value{}, before{};
        };

        // Main thread state.
        int g_enabled = -1; // ROAN_POSE, read once
        std::vector<Entry> g_entries; // scratch
        std::vector<Stored> g_stored;  // the running copy's
        std::unordered_set<uint32_t> g_copyMatrices; // scratch: the running copy's bone matrices
        Block g_block;
        Totals g_totals;

        // The object's bones as the pose copy reads them (82580968): entry i of out is the matrix the
        // copy reads for bone i (0 for the identity it draws instead).
        Found Resolve(uint32_t object, Layout& layout, std::vector<Entry>& out)
        {
            out.clear();
            uint32_t identity = mem::Load32(object + jade::object::kIdentity);
            if ((identity & jade::object::kIdentityAdditionalMatrices) == 0)
                return Found::kNone;
            uint32_t base = mem::Load32(object + jade::object::kBase);
            uint32_t additional = base != 0 ? mem::Load32(base + jade::base::kAdditionalMatrices) : 0;
            if (additional == 0)
                return Found::kNone;
            int32_t count = int32_t(mem::Load32(additional + jade::additional::kCount));
            uint32_t array = mem::Load32(additional + jade::additional::kArray);
            if (count <= 0 || array == 0)
                return Found::kNone;
            if (count > kMaxBones)
                return Found::kTooMany;
            layout.array = array;
            layout.count = uint32_t(count);
            layout.pointers = (identity & jade::object::kIdentityAdditionalPointers) != 0;
            out.resize(layout.count);
            for (uint32_t i = 0; i < layout.count; i++)
            {
                Entry& entry = out[i];
                if (!layout.pointers)
                {
                    entry.matrix = array + i * jade::matrix::kSize;
                    continue;
                }
                uint32_t gizmo = array + i * jade::gizmo::kSize;
                uint32_t boneObject = mem::Load32(gizmo + jade::gizmo::kObject);
                bool global = mem::Load32(gizmo + jade::gizmo::kId) == jade::gizmo::kIdGlobal
                    || (boneObject != 0
                        && (mem::Load32(boneObject + jade::object::kIdentity) & jade::object::kIdentityAdditionalPointers) != 0);
                if (global)
                {
                    // Without the bone's object the copy draws the identity.
                    if (boneObject != 0)
                        entry = { mem::Load32(boneObject + jade::object::kMatrix), boneObject, true };
                }
                else
                    entry = { mem::Load32(gizmo + jade::gizmo::kMatrix), boneObject, false };
            }
            return Found::kBones;
        }

        void Watch(const Skeleton& skeleton)
        {
            uint32_t stride = skeleton.pointers ? jade::gizmo::kSize : jade::matrix::kSize;
            lifetime::Watch(skeleton.array, skeleton.array + skeleton.count * stride);
            for (const Bone& bone : skeleton.bones)
            {
                if (bone.matrix != 0)
                    lifetime::Watch(bone.matrix, bone.matrix + jade::matrix::kSize);
                if (bone.object != 0)
                    lifetime::Watch(bone.object, bone.object + kObjectExtent);
            }
        }

        // A bone's position and rows in its owner's space: a global matrix is projected on the
        // owner's rows made unit (the owner's own scale and motion left out); a relative one is
        // already there.
        struct Local
        {
            Vector position;
            std::array<Vector, 3> rows;
        };

        Local InOwnerSpace(const Bits& bone, bool global, const Bits& owner)
        {
            Local local;
            if (!global)
            {
                local.position = matrices::Row(bone.data(), 3);
                for (uint32_t r = 0; r < 3; r++)
                    local.rows[r] = matrices::Row(bone.data(), r);
                return local;
            }
            std::array<Vector, 3> axes;
            for (uint32_t r = 0; r < 3; r++)
            {
                axes[r] = matrices::Row(owner.data(), r);
                float length = matrices::Length(axes[r]);
                if (length > 1e-6f)
                    for (float& c : axes[r])
                        c /= length;
            }
            auto project = [&](const Vector& v) {
                return Vector{ matrices::Dot(v, axes[0]), matrices::Dot(v, axes[1]), matrices::Dot(v, axes[2]) };
            };
            Vector at = matrices::Row(bone.data(), 3), origin = matrices::Row(owner.data(), 3);
            local.position = project({ at[0] - origin[0], at[1] - origin[1], at[2] - origin[2] });
            for (uint32_t r = 0; r < 3; r++)
                local.rows[r] = project(matrices::Row(bone.data(), r));
            return local;
        }

        template <size_t N>
        void Count(std::array<uint64_t, N>& buckets, const float (&limits)[N - 1], float value)
        {
            size_t i = 0;
            while (i < N - 1 && value >= limits[i])
                i++;
            buckets[i]++;
        }

        // A bone the display changed since its capture, left as the display wrote it: logged the first
        // time for each skeleton and bone (kLoggedLeftBones in all), so a scene that leaves bones names
        // them (one long fight left about 68 per view block; step 7 document, section 1).
        constexpr size_t kLoggedLeftBones = 32;
        std::vector<uint64_t> g_loggedLeft;

        void LogLeftBone(uint32_t object, size_t entry, uint32_t named, const Owner& owner, const Bone& bone, const Bits& live)
        {
            uint32_t array = owner.skeleton != nullptr ? owner.skeleton->array : 0;
            uint64_t key = uint64_t(array) << 32 | bone.matrix;
            if (g_loggedLeft.size() >= kLoggedLeftBones
                || std::find(g_loggedLeft.begin(), g_loggedLeft.end(), key) != g_loggedLeft.end())
                return;
            g_loggedLeft.push_back(key);
            float largest = 0.0f;
            for (uint32_t f = 0; f < matrices::kFloats; f++)
                largest = std::max(largest, std::fabs(Float(live[f]) - Float(bone.current[f])));
            bool pointers = owner.skeleton != nullptr && owner.skeleton->pointers;
            int32_t index = array != 0 && bone.matrix >= array && !pointers
                ? int32_t((bone.matrix - array) / jade::matrix::kSize)
                : -1;
            Log("pose: bone %08X left as the display wrote it (copy of %08X, entry %zu naming %08X; skeleton %08X, %u "
                "bones, %s, index %d, global %d; shown %d): largest float change %g, translation %g %g %g, captured %g %g %g",
                bone.matrix, object, entry, named, array,
                owner.skeleton != nullptr ? uint32_t(owner.skeleton->bones.size()) : 0u,
                pointers ? "pointer entries" : "68-byte bones", index, int(bone.global), int(owner.show), largest,
                Float(live[12]), Float(live[13]), Float(live[14]), Float(bone.current[12]),
                Float(bone.current[13]), Float(bone.current[14]));
        }
    }

    bool Enabled()
    {
        if (g_enabled < 0)
        {
            g_enabled = EnvironmentOff("ROAN_POSE") ? 0 : 1;
            if (g_enabled == 0)
                Log("pose: off (ROAN_POSE=0)");
        }
        return g_enabled == 1;
    }

    Decision Capture(Skeleton& skeleton, uint32_t object, const Bits& ownerPrevious, const Bits& ownerCurrent,
        bool ownerContinuing, const std::unordered_set<uint32_t>& written)
    {
        Layout layout;
        Found found = Resolve(object, layout, g_entries);
        // A bone the transforms wrote in this view block (a first capture during the display) does not
        // hold its tick's value: no skeleton until the next tick's capture, which leaves the actor at
        // the tick for that interval (a new skeleton, below).
        if (found == Found::kBones
            && std::any_of(g_entries.begin(), g_entries.end(), [&](const Entry& e) { return written.count(e.matrix) != 0; }))
        {
            skeleton = {};
            return ownerContinuing ? Decision::kTick : Decision::kContinue;
        }
        if (found != Found::kBones)
        {
            skeleton = {};
            if (found == Found::kNone)
                return Decision::kContinue;
            g_totals.tickLayout++;
            return Decision::kTick;
        }

        bool same = skeleton.array == layout.array && skeleton.count == layout.count
            && skeleton.pointers == layout.pointers && skeleton.bones.size() == g_entries.size();
        for (size_t i = 0; same && i < g_entries.size(); i++)
        {
            const Bone& bone = skeleton.bones[i];
            same = g_entries[i] == Entry{ bone.matrix, bone.object, bone.global };
        }
        if (!same)
        {
            skeleton.array = layout.array;
            skeleton.count = layout.count;
            skeleton.pointers = layout.pointers;
            skeleton.continuing = false;
            skeleton.captures = 1;
            skeleton.bones.assign(g_entries.size(), {});
            for (size_t i = 0; i < g_entries.size(); i++)
            {
                Bone& bone = skeleton.bones[i];
                bone.matrix = g_entries[i].matrix;
                bone.object = g_entries[i].object;
                bone.global = g_entries[i].global;
                if (bone.matrix != 0)
                    bone.current = matrices::LoadBits(bone.matrix);
            }
            Watch(skeleton);
            g_totals.starts++;
            // A new skeleton under an object that continues: no previous pose to blend from.
            if (ownerContinuing)
            {
                g_totals.tickLayout++;
                return Decision::kTick;
            }
            return Decision::kContinue;
        }

        skeleton.continuing = ownerContinuing;
        skeleton.captures++;
        bool invalid = false, jump = false, turn = false;
        float largestStep = 0.0f, largestTurn = 0.0f;
        for (Bone& bone : skeleton.bones)
        {
            if (bone.matrix == 0)
                continue;
            bone.previous = bone.current;
            bone.current = matrices::LoadBits(bone.matrix);
            if (!skeleton.continuing)
            {
                bone.lastStep = -1.0f;
                bone.lastAngle = -1.0f;
                continue;
            }
            if (!matrices::Finite(bone.previous.data(), matrices::kFloats)
                || !matrices::Finite(bone.current.data(), matrices::kFloats))
            {
                invalid = true;
                bone.lastStep = -1.0f;
                bone.lastAngle = -1.0f;
                continue;
            }
            Local from = InOwnerSpace(bone.previous, bone.global, ownerPrevious);
            Local to = InOwnerSpace(bone.current, bone.global, ownerCurrent);
            float step = matrices::Distance(from.position, to.position);
            float cosine = 1.0f;
            for (uint32_t r = 0; r < 3; r++)
                cosine = std::min(cosine, matrices::Cosine(from.rows[r], to.rows[r]));
            float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f));
            if (!std::isfinite(step) || !std::isfinite(angle))
            {
                invalid = true;
                bone.lastStep = -1.0f;
                bone.lastAngle = -1.0f;
                continue;
            }
            jump |= bone.lastStep < 0 ? step > kFirstJump : step > kJumpFloor && step > kJumpRatio * bone.lastStep;
            turn |= cosine < kTurnCosine && (bone.lastAngle < 0 || angle > kJumpRatio * bone.lastAngle);
            // Kept after a cut too (as for objects): a steady fast mover is compared with its own steps.
            bone.lastStep = step;
            bone.lastAngle = angle;
            largestStep = std::max(largestStep, step);
            largestTurn = std::max(largestTurn, angle);
        }
        if (!skeleton.continuing)
            return Decision::kContinue;

        g_totals.captures++;
        float degrees = largestTurn * 180.0f / std::numbers::pi_v<float>;
        Count(g_totals.steps, kStepBuckets, largestStep);
        Count(g_totals.turns, kTurnBuckets, degrees);
        g_totals.maxStep = std::max(g_totals.maxStep, double(largestStep));
        g_totals.maxTurn = std::max(g_totals.maxTurn, double(degrees));
        if (invalid)
        {
            // A bone not finite cannot be shown in place of the live one: the actor stays at the tick.
            skeleton.continuing = false;
            g_totals.tickInvalid++;
            return Decision::kTick;
        }
        if (!jump && !turn)
            return Decision::kContinue;
        (jump ? g_totals.cutJump : g_totals.cutTurn)++;
        // Bones relative to the object are drawn relative to its matrix: the pose can hold while the
        // object moves on. A global bone is made relative to the object by the copy, so it holds only
        // with the object.
        bool global = std::any_of(skeleton.bones.begin(), skeleton.bones.end(), [](const Bone& b) { return b.global; });
        if (global)
            return Decision::kCut;
        g_totals.poseHeld++;
        return Decision::kHoldPose;
    }

    void Walked(Skeleton& skeleton, uint32_t child, uint32_t matrix)
    {
        // The walk writes the 68-byte bones of the animated object (array + 68 k).
        if (skeleton.bones.empty() || skeleton.pointers || matrix < skeleton.array
            || (matrix - skeleton.array) % jade::matrix::kSize != 0)
            return;
        uint32_t index = (matrix - skeleton.array) / jade::matrix::kSize;
        if (index >= skeleton.bones.size() || skeleton.bones[index].matrix != matrix)
            return;
        // Only the paths from the child's local (826B96D8: a hierarchy child without status bit 31).
        if ((mem::Load32(child + jade::object::kIdentity) & jade::object::kIdentityHierarchy) == 0
            || (mem::Load32(child + jade::object::kStatus) & jade::object::kStatusGlobalToBone) != 0)
            return;
        Bone& bone = skeleton.bones[index];
        Bits live = matrices::LoadBits(matrix);
        // Not set by the walk at the previous capture: previous is not the tick before.
        bool fresh = bone.walked + 1 == skeleton.captures;
        bone.walked = skeleton.captures;
        if (!fresh)
            bone.previous = live;
        if (live == bone.current)
            return;
        bone.current = live;
        g_totals.walked++;
    }

    bool Touched(const Skeleton& skeleton, bool (*touches)(uint32_t begin, uint32_t end))
    {
        if (skeleton.bones.empty())
            return false;
        uint32_t stride = skeleton.pointers ? jade::gizmo::kSize : jade::matrix::kSize;
        if (touches(skeleton.array, skeleton.array + skeleton.count * stride))
            return true;
        return std::any_of(skeleton.bones.begin(), skeleton.bones.end(), [&](const Bone& bone) {
            return (bone.matrix != 0 && touches(bone.matrix, bone.matrix + jade::matrix::kSize))
                || (bone.object != 0 && touches(bone.object, bone.object + kObjectExtent));
        });
    }

    void CopyStart(uint32_t object, float alpha, FindOwner find, const std::unordered_set<uint32_t>& objectMatrices)
    {
        g_stored.clear();
        if (!Enabled())
            return;
        // The bones as the copy will read them.
        Layout layout;
        if (Resolve(object, layout, g_entries) != Found::kBones)
            return;
        Owner self = find(object);
        bool own = self.skeleton != nullptr && !self.skeleton->bones.empty();
        bool sameLayout = own && layout.array == self.skeleton->array && layout.count == self.skeleton->count
            && layout.pointers == self.skeleton->pointers;
        bool partial = false, found = false, borrowed = false;
        Show shown = Show::kTick;
        uint32_t lastObject = 0;
        Owner last;
        g_copyMatrices.clear();
        for (size_t i = 0; i < g_entries.size(); i++)
        {
            const Entry& entry = g_entries[i];
            if (entry.matrix == 0)
                continue;
            // The bone this entry reads, and its owner. A skin's entry first: a bone of the object it
            // names, by its address in that object's 68-byte matrices, so the skin follows that
            // object's decision even when the skin is tracked itself.
            const Bone* bone = nullptr;
            Owner owner;
            if (entry.object != 0 && entry.object != object && !entry.global)
            {
                if (entry.object != lastObject)
                {
                    lastObject = entry.object;
                    last = find(entry.object);
                }
                const Skeleton* skeleton = last.skeleton;
                if (skeleton != nullptr && !skeleton->pointers && entry.matrix >= skeleton->array
                    && (entry.matrix - skeleton->array) % jade::matrix::kSize == 0
                    && (entry.matrix - skeleton->array) / jade::matrix::kSize < skeleton->bones.size())
                {
                    owner = last;
                    bone = &skeleton->bones[(entry.matrix - skeleton->array) / jade::matrix::kSize];
                    borrowed = true;
                }
            }
            if (bone == nullptr && own)
            {
                // An entry naming another matrix than the capture's is not the bone that was captured.
                owner = self;
                const Bone* captured = sameLayout ? &self.skeleton->bones[i] : nullptr;
                if (captured == nullptr || !(entry == Entry{ captured->matrix, captured->object, captured->global }))
                {
                    g_totals.moved++;
                    partial = true;
                    continue;
                }
                bone = captured;
            }
            if (bone == nullptr)
                continue;
            found = true;
            shown = owner.show;
            // A matrix two entries name is written once.
            if (owner.show == Show::kTick || !owner.skeleton->continuing || !g_copyMatrices.insert(bone->matrix).second
                || bone->previous == bone->current)
                continue;
            if (objectMatrices.count(bone->matrix) != 0)
            {
                g_totals.shared++;
                partial = true;
                continue;
            }
            Bits live = matrices::LoadBits(bone->matrix);
            if (live != bone->current)
            {
                // Only the display writes between ticks: its value is drawn.
                g_totals.display++;
                LogLeftBone(object, i, entry.object, owner, *bone, live);
                partial = true;
                continue;
            }
            Bits target;
            if (owner.show == Show::kPrevious)
                target = bone->previous;
            else
                matrices::Blend(bone->previous.data(), bone->current.data(), alpha, target.data());
            Stored stored;
            stored.matrix = bone->matrix;
            stored.before = live;
            for (uint32_t f = 0; f < matrices::kFloats; f++)
            {
                if (target[f] == live[f])
                    continue;
                mem::Store32(bone->matrix + 4 * f, target[f]);
                stored.value[f] = target[f];
                stored.mask |= 1u << f;
            }
            if (stored.mask != 0)
                g_stored.push_back(stored);
        }
        if (!found)
        {
            g_totals.untracked++;
            g_block.untracked++;
            return;
        }
        if (borrowed)
            g_totals.borrowed++;
        if (partial)
            g_block.partial++;
        if (g_stored.empty())
            return;
        g_totals.bones += g_stored.size();
        g_block.bones += uint32_t(g_stored.size());
        if (shown == Show::kPrevious)
        {
            g_totals.held++;
            g_block.held++;
        }
        else
        {
            g_totals.blended++;
            g_block.blended++;
        }
    }

    void CopyEnd()
    {
        for (const Stored& stored : g_stored)
            for (uint32_t f = 0; f < matrices::kFloats; f++)
            {
                if ((stored.mask & (1u << f)) == 0)
                    continue;
                uint32_t address = stored.matrix + 4 * f;
                if (mem::Load32(address) == stored.value[f])
                    mem::Store32(address, stored.before[f]);
                else
                    g_totals.unrestored++;
            }
        g_stored.clear();
    }

    void BlockStart()
    {
        g_block = {};
    }

    void NoteCut()
    {
        g_block.cuts++;
    }

    const Block& LastBlock()
    {
        return g_block;
    }

    Totals Totals::operator-(const Totals& start) const
    {
        Totals out = *this;
        out.captures -= start.captures;
        out.starts -= start.starts;
        out.cutJump -= start.cutJump;
        out.cutTurn -= start.cutTurn;
        out.poseHeld -= start.poseHeld;
        out.tickLayout -= start.tickLayout;
        out.tickInvalid -= start.tickInvalid;
        out.blended -= start.blended;
        out.held -= start.held;
        out.bones -= start.bones;
        out.untracked -= start.untracked;
        out.borrowed -= start.borrowed;
        out.walked -= start.walked;
        out.display -= start.display;
        out.moved -= start.moved;
        out.shared -= start.shared;
        out.unrestored -= start.unrestored;
        for (size_t i = 0; i < steps.size(); i++)
            out.steps[i] -= start.steps[i];
        for (size_t i = 0; i < turns.size(); i++)
            out.turns[i] -= start.turns[i];
        return out;
    }

    Totals Current()
    {
        return g_totals;
    }
}
