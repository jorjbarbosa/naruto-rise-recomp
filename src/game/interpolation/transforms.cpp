// Objects and camera between ticks (transforms.h).

#include "game/interpolation/transforms.h"

#include "game/addresses.h"
#include "game/interpolation/clock.h"
#include "game/interpolation/diagnostics.h"
#include "game/interpolation/lifetime.h"
#include "game/interpolation/matrices.h"
#include "game/interpolation/pose.h"
#include "game/interpolation/view_clock.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace game::interpolation::transforms
{
    namespace
    {
        // Cuts of objects, per tick. A row turning more than 60 degrees (the reference's). A position
        // step is a jump when it is above kJumpFloor and kJumpRatio times the step of the tick before;
        // without one (a track's first interval) above the reference's 4 units. So a steady fast mover
        // keeps interpolating (plan F6) and a teleport from rest or from a walk is still a cut.
        constexpr float kTurnCosine = 0.5f;
        constexpr float kFirstJump = 4.0f;
        constexpr float kJumpRatio = 4.0f;
        constexpr float kJumpFloor = 1.0f;
        // Cuts of the camera: the detector's thresholds (8267A2A0), 10 units or a row turning 30
        // degrees in a tick, when the step is also a jump against the step of the tick before (as for
        // objects): a fly-in (a fight's intro) passes the thresholds in every tick.
        constexpr float kCameraDistance = 10.0f;
        constexpr float kCameraCosine = 0.866f;

        using matrices::Bits;
        using matrices::Bits32;
        using matrices::Blend;
        using matrices::Distance;
        using matrices::Finite;
        using matrices::Float;
        using matrices::kFloats;
        using matrices::LoadBits;
        using matrices::Row;
        using matrices::SmallestCosine;
        using lifetime::kObjectExtent;

        constexpr uint32_t kCameraFloats = kFloats + 1; // the matrix and the field of view
        constexpr uint32_t kTranslationFloats = 0x7000; // floats 12 to 14
        constexpr uint32_t kViewEntrySize = 172;

        using CameraBits = std::array<uint32_t, kCameraFloats>;

        enum CameraCut : uint32_t
        {
            kCameraCutMove = 1,     // the detector's thresholds
            kCameraCutFather = 2,   // another object followed
            kCameraCutViewport = 4,
            kCameraCutInvalid = 8,  // a float not finite, or a field of view not above 0
        };

        struct Track
        {
            uint32_t matrix = 0;     // [object+20] when captured
            uint64_t tick = 0;       // the tick of current (clock::Frame::ticks)
            uint64_t seen = 0;       // the last tick interval the display rendered the object in
            bool continuing = false; // previous holds the tick before
            bool cut = false;        // previous to current is a discontinuity
            float lastStep = -1.0f;  // the position step of the interval before, -1 unknown
            // Floats the display writes (found changed after our store, or since the tick): never
            // written. The display animates them itself, per call, by the scaled dt (modifiers 42 and
            // 45 integrate in the matrix, modifier 5 turns it), so a blend under them would roll its
            // motion back to the previous tick's and the logic would read that back.
            uint32_t displayFloats = 0;
            Bits previous{}, current{};
            uint32_t previousType = 0, currentType = 0;
            pose::Skeleton skeleton; // captured and decided with the object (pose.h)
            bool poseHeld = false;   // the bones show the previous tick (pose::Decision::kHoldPose)
        };

        struct CameraTrack
        {
            bool known = false;
            uint64_t tick = 0;
            bool continuing = false;
            uint32_t cut = 0; // CameraCut bits
            // The position step and the largest row turn (radians) of the interval before; -1 unknown.
            float lastDistance = -1.0f, lastAngle = -1.0f;
            CameraBits previous{}, current{};
            std::array<uint32_t, 4> viewport{};
            uint32_t father = 0;
        };

        // Floats stored over the live ones, put back after the delivery.
        struct Written
        {
            uint32_t address = 0; // the 16 floats
            // The 17th word: the camera's field of view, or an object matrix's type word (written only
            // across a change of the scale bit, WriteObjects), or 0.
            uint32_t extra = 0;
            uint32_t end = 0;     // the end of the structure, for the lifetime reports
            uint32_t object = 0;  // the object, 0 for a camera
            uint32_t mask = 0;    // the floats stored (bit i: float i)
            CameraBits value{}, before{};

            uint32_t Address(uint32_t i) const { return i < kFloats ? address + 4 * i : extra; }
        };

        // Main thread state.
        int g_enabled = -1;     // ROAN_TRANSFORMS, read once
        Suspended g_suspended = Suspended::kNone;
        bool g_active = false;  // the current view block is under the gate
        bool g_tick = false;    // ... and the logic ran since the last one
        bool g_any = false;     // a view block of g_timeline ran
        uint32_t g_timeline = 0;
        uint64_t g_ticks = 0;   // clock::Frame::ticks of the last view block
        double g_alpha = 0;
        std::unordered_map<uint32_t, Track> g_tracks;        // by object
        std::unordered_map<uint32_t, CameraTrack> g_cameras; // by view entry
        std::vector<Written> g_written;
        std::vector<Written> g_replay;   // the last view block's, for ReplayScope
        std::vector<Written> g_replayed; // what a ReplayScope stored
        std::unordered_set<uint32_t> g_writtenMatrices; // this view block's
        std::unordered_set<uint32_t> g_camerasDone;     // this view block's
        std::vector<lifetime::Range> g_ranges;          // the last drain's, sorted and merged
        bool g_lostAll = false;                         // the last drain lost reports
        Block g_block;
        Totals g_totals;

        bool Enabled()
        {
            if (g_enabled < 0)
            {
                g_enabled = EnvironmentOff("ROAN_TRANSFORMS") ? 0 : 1;
                if (g_enabled == 0)
                    Log("transforms: off (ROAN_TRANSFORMS=0)");
            }
            return g_enabled == 1;
        }

        void Clear()
        {
            g_tracks.clear();
            g_cameras.clear();
            lifetime::Unwatch();
        }

        // The reported ranges, merged into a sorted disjoint list: whether one touches [begin, end).
        bool Touches(uint32_t begin, uint32_t end)
        {
            auto it = std::upper_bound(g_ranges.begin(), g_ranges.end(), begin,
                [](uint32_t value, const lifetime::Range& range) { return value < range.end; });
            return it != g_ranges.end() && it->begin < end;
        }

        // Drops what the lifetime reports touch: tracks, cameras, and the floats to put back.
        void DropFreed()
        {
            bool everything = false;
            lifetime::Drain(g_ranges, everything);
            g_lostAll = everything;
            // Reports were lost: nothing tracked is known to be alive. The floats to put back stay: the
            // restore stores only where our floats are still there, and leaving a blend in a live
            // object would hand it to the logic.
            if (everything)
            {
                g_totals.dropped += g_tracks.size();
                g_replay.clear();
                Clear();
                return;
            }
            if (g_ranges.empty())
                return;
            std::sort(g_ranges.begin(), g_ranges.end(),
                [](const lifetime::Range& a, const lifetime::Range& b) { return a.begin < b.begin; });
            size_t merged = 0;
            for (const lifetime::Range& range : g_ranges)
            {
                if (merged != 0 && range.begin <= g_ranges[merged - 1].end)
                    g_ranges[merged - 1].end = std::max(g_ranges[merged - 1].end, range.end);
                else
                    g_ranges[merged++] = range;
            }
            g_ranges.resize(merged);

            // A skeleton touched drops its object too: the actor is drawn at the tick (pose.h).
            std::erase_if(g_tracks, [](const auto& item) {
                bool touched = Touches(item.first, item.first + kObjectExtent)
                    || Touches(item.second.matrix, item.second.matrix + jade::matrix::kSize)
                    || pose::Touched(item.second.skeleton, Touches);
                if (touched)
                    g_totals.dropped++;
                return touched;
            });
            std::erase_if(g_cameras, [](const auto& item) { return Touches(item.first, item.first + kViewEntrySize); });
            auto touched = [](const Written& w) {
                return Touches(w.address, w.end) || (w.object != 0 && Touches(w.object, w.object + kObjectExtent));
            };
            std::erase_if(g_written, [&](const Written& w) {
                if (!touched(w))
                    return false;
                g_totals.unrestored += std::popcount(w.mask);
                return true;
            });
            std::erase_if(g_replay, touched);
        }

        // Stores the floats of a write where the other side's are there; the floats stored are the
        // result's mask.
        Written Swap(const Written& written, const std::array<uint32_t, kCameraFloats>& from,
            const std::array<uint32_t, kCameraFloats>& to)
        {
            Written out = written;
            out.mask = 0;
            for (uint32_t i = 0; i < kCameraFloats; i++)
            {
                if ((written.mask & (1u << i)) == 0)
                    continue;
                uint32_t address = written.Address(i);
                if (mem::Load32(address) != from[i])
                    continue;
                mem::Store32(address, to[i]);
                out.mask |= 1u << i;
            }
            return out;
        }

        // The cut between a track's two ticks.
        void DecideCut(Track& track)
        {
            track.cut = false;
            if (!Finite(track.previous.data(), kFloats) || !Finite(track.current.data(), kFloats))
            {
                // Neither end can be shown in place of the live matrix: not written.
                track.cut = true;
                track.continuing = false;
                track.lastStep = -1.0f;
                g_totals.cutInvalid++;
                return;
            }
            // The fourth column changes meaning: blended in the live type's convention (WriteObjects).
            if (((track.previousType ^ track.currentType) & jade::matrix::kTypeScale) != 0)
                g_totals.scaleChanges++;
            float step = Distance(Row(track.previous.data(), 3), Row(track.current.data(), 3));
            g_totals.maxStep = std::max(g_totals.maxStep, double(step));
            bool jump = track.lastStep < 0 ? step > kFirstJump : step > kJumpFloor && step > kJumpRatio * track.lastStep;
            bool turn = SmallestCosine(track.previous.data(), track.current.data()) < kTurnCosine;
            if (jump)
                g_totals.cutJump++;
            else if (turn)
                g_totals.cutTurn++;
            else if (step > kFirstJump)
                g_totals.fastKept++;
            track.cut = jump || turn;
            // Kept after a cut too: a steady fast mover is cut once (its first step is unknown), then
            // compared with its own steps.
            track.lastStep = step;
        }

        // The object's skeleton at its tick, decided with it (pose.h): a bone's jump holds the pose,
        // or the whole actor when a bone is global; a skeleton that cannot be blended leaves the actor
        // at the tick.
        void CaptureSkeleton(uint32_t object, Track& track)
        {
            track.poseHeld = false;
            if (!pose::Enabled())
                return;
            pose::Decision decision = pose::Capture(track.skeleton, object, track.previous, track.current, track.continuing,
                g_writtenMatrices);
            if (!track.continuing)
                return;
            switch (decision)
            {
            case pose::Decision::kContinue:
                break;
            case pose::Decision::kTick:
                track.continuing = false;
                break;
            case pose::Decision::kHoldPose:
                pose::NoteCut();
                track.poseHeld = true;
                break;
            case pose::Decision::kCut:
                pose::NoteCut();
                if (!track.cut)
                {
                    track.cut = true;
                    g_block.cuts++;
                }
                break;
            }
        }

        // What the actor of a track shows in this view block (pose::Show): WriteObjects' decision for
        // the object, whether or not its matrix needed a write.
        pose::Show Shows(uint32_t object, const Track& track)
        {
            // The live type word: the tick's, or the one WriteObjects stored across a change of the
            // scale bit.
            uint32_t type = mem::Load32(track.matrix + jade::matrix::kType);
            bool scaleChange = ((track.previousType ^ track.currentType) & jade::matrix::kTypeScale) != 0;
            if (!track.continuing || track.tick != g_ticks
                || mem::Load32(object + jade::object::kMatrix) != track.matrix
                || (type != track.currentType && !(scaleChange && type == (track.currentType | jade::matrix::kTypeScale))))
                return pose::Show::kTick;
            return track.cut ? pose::Show::kPrevious : pose::Show::kBlend;
        }

        // The skeleton of a tracked object and what its bones show (pose::FindOwner).
        pose::Owner FindOwner(uint32_t object)
        {
            auto it = g_tracks.find(object);
            if (it == g_tracks.end())
                return {};
            pose::Show show = Shows(object, it->second);
            if (show == pose::Show::kBlend && it->second.poseHeld)
                show = pose::Show::kPrevious;
            return { &it->second.skeleton, show };
        }

        // At the first view block after a tick: each object the display rendered in the last interval
        // moves to this tick; the others are dropped.
        void CaptureObjects()
        {
            for (auto it = g_tracks.begin(); it != g_tracks.end();)
            {
                uint32_t object = it->first;
                Track& track = it->second;
                uint32_t matrix = mem::Load32(object + jade::object::kMatrix);
                if (track.seen + 1 < g_ticks || matrix == 0)
                {
                    it = g_tracks.erase(it);
                    continue;
                }
                Bits live = LoadBits(matrix);
                uint32_t type = mem::Load32(matrix + jade::matrix::kType);
                if (matrix == track.matrix && track.tick + 1 == g_ticks)
                {
                    track.previous = track.current;
                    track.previousType = track.currentType;
                    track.continuing = true;
                    g_totals.captures++;
                }
                else
                {
                    if (matrix != track.matrix && track.tick + 1 == g_ticks)
                        g_totals.cutMatrix++;
                    track.matrix = matrix;
                    track.continuing = false;
                    track.lastStep = -1.0f;
                    track.displayFloats = 0;
                    lifetime::Watch(matrix, matrix + jade::matrix::kSize);
                    g_totals.starts++;
                }
                track.current = live;
                track.currentType = type;
                track.tick = g_ticks;
                if (track.continuing)
                {
                    DecideCut(track);
                    if (track.cut)
                        g_block.cuts++;
                }
                CaptureSkeleton(object, track);
                ++it;
            }
            std::erase_if(g_cameras, [](const auto& item) { return item.second.tick + 1 < g_ticks; });
        }

        void WriteObjects()
        {
            float alpha = float(g_alpha);
            for (auto& [object, track] : g_tracks)
            {
                if (!track.continuing || track.tick != g_ticks || track.previous == track.current)
                    continue;
                if (mem::Load32(object + jade::object::kMatrix) != track.matrix
                    || mem::Load32(track.matrix + jade::matrix::kType) != track.currentType)
                    continue;
                // A matrix shared by objects is written once, by the first track that stores in it.
                if (g_writtenMatrices.count(track.matrix) != 0)
                    continue;

                // Across a change of the scale bit, the engine's own blend (WOR_InterpoleMatrix_XX): the
                // end without the bit has scale 1 and the result has the bit. Written under the live
                // type word, a scale carried in the rows would not survive a modifier that turns them
                // (modifier 5 makes the first row unit): the type word is written too, and put back.
                Bits previous = track.previous, current = track.current;
                uint32_t type = track.currentType;
                if (((track.previousType ^ track.currentType) & jade::matrix::kTypeScale) != 0)
                {
                    matrices::UnitScale((track.previousType & jade::matrix::kTypeScale) != 0 ? current : previous);
                    type |= jade::matrix::kTypeScale;
                }
                Bits target;
                if (track.cut)
                    target = previous;
                else
                    Blend(previous.data(), current.data(), alpha, target.data());
                Written written;
                written.address = track.matrix;
                written.extra = track.matrix + jade::matrix::kType;
                written.end = track.matrix + jade::matrix::kSize;
                written.object = object;
                bool partial = false;
                for (uint32_t i = 0; i < kFloats; i++)
                {
                    uint32_t address = track.matrix + 4 * i;
                    uint32_t live = mem::Load32(address);
                    written.before[i] = live;
                    // Changed since the tick: only the display writes between ticks.
                    if (live != track.current[i])
                        track.displayFloats |= 1u << i;
                    if ((track.displayFloats & (1u << i)) != 0)
                    {
                        partial = true;
                        continue;
                    }
                    if (target[i] == live)
                        continue;
                    mem::Store32(address, target[i]);
                    written.value[i] = target[i];
                    written.mask |= 1u << i;
                }
                written.before[kFloats] = track.currentType;
                if (written.mask != 0 && type != track.currentType)
                {
                    mem::Store32(written.extra, type);
                    written.value[kFloats] = type;
                    written.mask |= 1u << kFloats;
                }
                if (partial)
                    g_totals.partial++;
                if (written.mask == 0)
                    continue;
                g_writtenMatrices.insert(track.matrix);
                g_written.push_back(written);
                g_totals.written++;
                g_block.written++;
                if (track.cut)
                {
                    g_totals.held++;
                    g_block.held++;
                }
            }
        }
    }

    void ViewBlockStart()
    {
        ViewBlockEnd();
        g_replay.clear();
        g_block = {};
        pose::BlockStart();
        g_camerasDone.clear();
        g_writtenMatrices.clear();
        view_clock::Times times = view_clock::CurrentTimes();
        if (!times.active || !Enabled() || g_suspended == Suspended::kAll)
        {
            if (g_any)
            {
                Clear();
                g_any = false;
            }
            return;
        }

        const clock::Frame& frame = clock::Current();
        bool sameTimeline = g_any && frame.timeline == g_timeline;
        g_tick = !sameTimeline || frame.ticks != g_ticks;
        // A new timeline, or ticks without a view block between them: nothing continues.
        if (!sameTimeline || (g_tick && frame.ticks != g_ticks + 1))
            Clear();
        g_any = true;
        g_timeline = frame.timeline;
        g_ticks = frame.ticks;
        g_alpha = times.alpha;
        g_active = true;
        g_totals.blocks++;

        DropFreed();
        if (g_suspended == Suspended::kObjects)
            g_tracks.clear();
        else
        {
            if (g_tick)
                CaptureObjects();
            WriteObjects();
        }
        g_block.active = true;
        g_block.alpha = g_alpha;
    }

    void ViewBlockEnd()
    {
        if (!g_active)
            return;
        g_active = false;
        DropFreed();
        for (const Written& written : g_written)
        {
            uint32_t changed = 0;
            for (uint32_t i = 0; i < kCameraFloats; i++)
            {
                if ((written.mask & (1u << i)) == 0)
                    continue;
                uint32_t address = written.Address(i);
                // A float the display wrote over ours is its own (step 6 names the writers).
                if (mem::Load32(address) == written.value[i])
                    mem::Store32(address, written.before[i]);
                else
                    changed |= 1u << i;
            }
            if (changed != 0)
                (written.object != 0 ? g_totals.changed : g_totals.cameraChanged)++;
            // Those floats are the display's from now on (Track::displayFloats).
            if (written.object != 0 && changed != 0)
            {
                auto track = g_tracks.find(written.object);
                if (track != g_tracks.end() && track->second.matrix == written.address)
                    track->second.displayFloats |= changed;
            }
            if (written.object != 0 && (changed & kTranslationFloats) != 0)
                g_totals.changedTranslation++;
        }
        g_replay.swap(g_written);
        g_written.clear();
    }

    bool Freed(uint32_t begin, uint32_t end)
    {
        return g_lostAll || Touches(begin, end);
    }

    void Suspend(Suspended what)
    {
        g_suspended = what;
    }

    ReplayScope::ReplayScope()
    {
        if (g_active)
            return;
        DropFreed();
        g_replayed.clear();
        for (const Written& written : g_replay)
        {
            Written stored = Swap(written, written.before, written.value);
            if (stored.mask != 0)
                g_replayed.push_back(stored);
        }
    }

    ReplayScope::~ReplayScope()
    {
        Restore();
    }

    void ReplayScope::Restore()
    {
        if (restored_)
            return;
        restored_ = true;
        for (const Written& stored : g_replayed)
            Swap(stored, stored.value, stored.before);
        g_replayed.clear();
    }

    void Camera(uint32_t entry)
    {
        if (!g_active || entry == 0 || !g_camerasDone.insert(entry).second)
            return;
        CameraTrack& camera = g_cameras[entry];
        CameraBits live;
        for (uint32_t i = 0; i < kFloats; i++)
            live[i] = mem::Load32(entry + jade::view_entry::kMatrix + 4 * i);
        live[kFloats] = mem::Load32(entry + jade::view_entry::kFieldOfView);

        if (g_tick)
        {
            std::array<uint32_t, 4> viewport;
            for (uint32_t i = 0; i < 4; i++)
                viewport[i] = mem::Load32(entry + jade::view_entry::kViewport + 4 * i);
            uint32_t father = mem::Load32(entry + jade::view_entry::kFather);
            bool continuing = camera.known && camera.tick + 1 == g_ticks;
            camera.previous = continuing ? camera.current : live;
            camera.current = live;
            uint32_t cut = 0;
            if (continuing)
            {
                if (father != camera.father)
                    cut |= kCameraCutFather;
                if (viewport != camera.viewport)
                    cut |= kCameraCutViewport;
                if (!Finite(camera.previous.data(), kCameraFloats) || !Finite(live.data(), kCameraFloats)
                    || !(Float(camera.previous[kFloats]) > 0.0f) || !(Float(live[kFloats]) > 0.0f))
                    cut |= kCameraCutInvalid;
                float distance = Distance(Row(camera.previous.data(), 3), Row(live.data(), 3));
                float cosine = SmallestCosine(camera.previous.data(), live.data());
                float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f));
                bool moves = distance >= kCameraDistance
                    && (camera.lastDistance < 0 || distance > kJumpRatio * camera.lastDistance);
                bool turns = cosine < kCameraCosine && (camera.lastAngle < 0 || angle > kJumpRatio * camera.lastAngle);
                if (moves || turns)
                    cut |= kCameraCutMove;
                else if (distance >= kCameraDistance || cosine < kCameraCosine)
                    g_totals.cameraFastKept++;
                // Kept after a cut too (as for objects), unless the cut is not a motion.
                bool known = (cut & ~kCameraCutMove) == 0 && std::isfinite(distance) && std::isfinite(angle);
                camera.lastDistance = known ? distance : -1.0f;
                camera.lastAngle = known ? angle : -1.0f;
            }
            else
            {
                camera.lastDistance = -1.0f;
                camera.lastAngle = -1.0f;
            }
            camera.known = true;
            // An end not finite (or a field of view not above 0) cannot be shown: not written.
            camera.continuing = continuing && (cut & kCameraCutInvalid) == 0;
            camera.cut = cut;
            camera.tick = g_ticks;
            camera.father = father;
            camera.viewport = viewport;
            g_block.cameraCut = cut;
            g_totals.cameraCutMove += (cut & kCameraCutMove) != 0;
            g_totals.cameraCutFather += (cut & kCameraCutFather) != 0;
            g_totals.cameraCutViewport += (cut & kCameraCutViewport) != 0;
            g_totals.cameraCutInvalid += (cut & kCameraCutInvalid) != 0;
            lifetime::Watch(entry, entry + kViewEntrySize);
        }
        if (!camera.continuing || camera.tick != g_ticks || camera.previous == camera.current)
            return;

        CameraBits target;
        if (camera.cut != 0)
            target = camera.previous;
        else
        {
            float alpha = float(g_alpha);
            Blend(camera.previous.data(), camera.current.data(), alpha, target.data());
            float from = Float(camera.previous[kFloats]);
            target[kFloats] = Bits32(from + (Float(camera.current[kFloats]) - from) * alpha);
        }
        Written written;
        written.address = entry + jade::view_entry::kMatrix;
        written.extra = entry + jade::view_entry::kFieldOfView;
        written.end = entry + kViewEntrySize;
        for (uint32_t i = 0; i < kCameraFloats; i++)
        {
            uint32_t address = written.Address(i);
            uint32_t value = live[i];
            written.before[i] = value;
            if (value != camera.current[i] || target[i] == value)
                continue;
            mem::Store32(address, target[i]);
            written.value[i] = target[i];
            written.mask |= 1u << i;
        }
        if (written.mask == 0)
            return;
        g_written.push_back(written);
        g_totals.cameraWritten++;
        g_block.camera = camera.cut != 0 ? 2 : 1;
        if (camera.cut != 0)
            g_totals.cameraHeld++;
    }

    void NoteRendered(uint32_t object)
    {
        if (!g_active || g_suspended == Suspended::kObjects)
            return;
        uint32_t matrix = mem::Load32(object + jade::object::kMatrix);
        auto it = g_tracks.find(object);
        if (it != g_tracks.end() && it->second.matrix == matrix && it->second.tick == g_ticks)
        {
            it->second.seen = g_ticks;
            return;
        }
        // A new track, at this tick's state: not when this view block wrote the matrix (another
        // object's, shared).
        if (matrix == 0 || g_writtenMatrices.count(matrix) != 0)
        {
            if (it != g_tracks.end())
                g_tracks.erase(it);
            return;
        }
        Track& track = g_tracks[object];
        track = {};
        track.matrix = matrix;
        track.tick = g_ticks;
        track.seen = g_ticks;
        track.current = LoadBits(matrix);
        track.currentType = mem::Load32(matrix + jade::matrix::kType);
        g_totals.starts++;
        lifetime::Watch(object, object + kObjectExtent);
        lifetime::Watch(matrix, matrix + jade::matrix::kSize);
        CaptureSkeleton(object, track);
    }

    void PoseCopyStart(uint32_t object)
    {
        if (!g_active || !pose::Enabled())
            return;
        pose::CopyStart(object, float(g_alpha), FindOwner, g_writtenMatrices);
    }

    void PoseCopyEnd()
    {
        if (g_active)
            pose::CopyEnd();
    }

    void SkeletonChildWalked(uint32_t child, uint32_t object, uint32_t bone)
    {
        // Only the view block of the capture: between ticks the walk recomputes the same value.
        if (!g_active || !g_tick || !pose::Enabled() || child == 0)
            return;
        auto it = g_tracks.find(object);
        if (it != g_tracks.end())
            pose::Walked(it->second.skeleton, child, bone);
    }

    const Block& LastBlock()
    {
        return g_block;
    }

    Totals Totals::operator-(const Totals& start) const
    {
        Totals out = *this;
        out.blocks -= start.blocks;
        out.starts -= start.starts;
        out.captures -= start.captures;
        out.written -= start.written;
        out.held -= start.held;
        out.partial -= start.partial;
        out.changed -= start.changed;
        out.changedTranslation -= start.changedTranslation;
        out.cameraFastKept -= start.cameraFastKept;
        out.cutJump -= start.cutJump;
        out.cutTurn -= start.cutTurn;
        out.cutMatrix -= start.cutMatrix;
        out.scaleChanges -= start.scaleChanges;
        out.cutInvalid -= start.cutInvalid;
        out.fastKept -= start.fastKept;
        out.dropped -= start.dropped;
        out.unrestored -= start.unrestored;
        out.cameraWritten -= start.cameraWritten;
        out.cameraHeld -= start.cameraHeld;
        out.cameraChanged -= start.cameraChanged;
        out.cameraCutMove -= start.cameraCutMove;
        out.cameraCutFather -= start.cameraCutFather;
        out.cameraCutViewport -= start.cameraCutViewport;
        out.cameraCutInvalid -= start.cameraCutInvalid;
        return out;
    }

    Totals Current()
    {
        return g_totals;
    }
}
