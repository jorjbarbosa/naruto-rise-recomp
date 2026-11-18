// The motion record of the displays of the main step (motion_record.h).

#include "game/interpolation/motion_record.h"

#include "game/addresses.h"
#include "game/interpolation/diagnostics.h"
#include "game/interpolation/pose.h"
#include "game/interpolation/transforms.h"
#include "game/interpolation/view_clock.h"
#include "game/interpolation/world.h"
#include "host/timing.h"
#include "log.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace game::interpolation::motion
{
    namespace
    {
        constexpr uint32_t kMaxActors = 32;    // objects with dynamics recorded per display
        constexpr uint32_t kBoneStride = 6;    // every 6th bone of a palette is recorded
        constexpr float kOwnerDistance = 3.0f; // a skinned object this close to an actor follows it
        constexpr uint64_t kRatePeriod = 120;  // main steps between two rate lines in the log

        struct Vector
        {
            float x = 0, y = 0, z = 0;
        };

        struct Actor
        {
            uint32_t object;
            Vector position;
            Vector row; // first row of its matrix
        };

        struct BoneRow
        {
            uint32_t owner, object, bone;
            Vector world, local;
            float distance;
        };

        // An object with modifier 5 (the look-at icons), with the scale of its first row as the view
        // block gave it, before the modifier turned it.
        struct LookAt
        {
            uint32_t object, matrix;
            float scale;
        };
        constexpr uint32_t kLookAtModifier = 5;
        constexpr uint32_t kMaxLookAts = 32;

        // Main thread state.
        int g_enabled = -1;
        std::FILE* g_file = nullptr;
        int64_t g_start = 0;
        uint64_t g_iteration = 0;
        bool g_tick = true;
        float g_alpha = 0.0f;
        uint64_t g_mainSteps = 0;
        uint64_t g_ticks = 0;
        uint64_t g_displays = 0;
        uint64_t g_rateSteps = 0;
        uint64_t g_rateTicks = 0;
        uint64_t g_rateDisplays = 0;
        int64_t g_rateStart = 0;
        bool g_inDisplay = false;
        uint32_t g_view = 0;
        uint32_t g_rendered = 0;
        std::vector<Actor> g_actors;         // the actors this display rendered, as it rendered them
        std::vector<Actor> g_previousActors; // the previous display's, for owners rendered after their bones
        std::vector<BoneRow> g_bones;
        std::vector<LookAt> g_lookAts;

        // The scale of a matrix's row r: its length, times its fourth float with the scale bit.
        float RowScale(uint32_t matrix, uint32_t r)
        {
            float x = LoadFloat(matrix + 16 * r), y = LoadFloat(matrix + 16 * r + 4), z = LoadFloat(matrix + 16 * r + 8);
            float length = std::sqrt(x * x + y * y + z * z);
            if ((mem::Load32(matrix + jade::matrix::kType) & jade::matrix::kTypeScale) != 0)
                length *= LoadFloat(matrix + 16 * r + 12);
            return std::isfinite(length) ? length : 0.0f;
        }

        int64_t Microseconds()
        {
            return (host::Counter() - g_start) * 1000000 / host::CounterFrequency();
        }

        float Finite(float value)
        {
            return std::isfinite(value) ? value : 0.0f;
        }

        Vector LoadVector(uint32_t address)
        {
            return { Finite(LoadFloat(address)), Finite(LoadFloat(address + 4)), Finite(LoadFloat(address + 8)) };
        }

        // A point through an engine matrix (row vectors; with the scale bit, row r scaled by its
        // fourth float).
        Vector Transform(uint32_t matrix, const Vector& point)
        {
            bool scaled = (mem::Load32(matrix + jade::matrix::kType) & jade::matrix::kTypeScale) != 0;
            const float in[3] = { point.x, point.y, point.z };
            float out[3];
            for (uint32_t column = 0; column < 3; column++)
            {
                float value = LoadFloat(matrix + jade::matrix::kTranslation + 4 * column);
                for (uint32_t row = 0; row < 3; row++)
                {
                    float scale = scaled ? LoadFloat(matrix + 16 * row + 12) : 1.0f;
                    value += in[row] * LoadFloat(matrix + 16 * row + 4 * column) * scale;
                }
                out[column] = Finite(value);
            }
            return { out[0], out[1], out[2] };
        }

        // The row prefix: display, us, iteration, tick, alpha, cut.
        void WritePrefix(int64_t us, uint32_t cut)
        {
            std::fprintf(g_file, "%llu,%lld,%llu,%u,%.4f,%u,", (unsigned long long)g_displays, (long long)us,
                (unsigned long long)g_iteration, g_tick ? 1u : 0u, g_alpha, cut);
        }

        void LogRates()
        {
            int64_t now = Microseconds();
            double seconds = double(now - g_rateStart) * 1e-6;
            uint32_t time = TimeStructure();
            if (seconds > 0 && time != 0)
            {
                Log("motion: %.1f main steps/s, %.1f ticks/s, %.1f displays/s, fixed step %u, period %.4f s, dt %.4f s",
                    double(g_mainSteps - g_rateSteps) / seconds, double(g_ticks - g_rateTicks) / seconds,
                    double(g_displays - g_rateDisplays) / seconds,
                    mem::Load32(time + jade::tim::kFixedStep), LoadFloat(time + jade::tim::kPeriod),
                    LoadFloat(time + jade::tim::kDtScaled));
            }
            g_rateStart = now;
            g_rateSteps = g_mainSteps;
            g_rateTicks = g_ticks;
            g_rateDisplays = g_displays;
        }
    }

    bool Enabled()
    {
        if (g_enabled < 0)
        {
            g_enabled = 0;
            if (EnvironmentFlag("ROAN_MOTION_RECORD"))
            {
                g_file = OpenDiagnosticsFile("motion.csv");
                if (g_file != nullptr)
                {
                    std::fputs("display,us,iteration,tick,alpha,cut,kind,id,x,y,z,a,b,c,d\n", g_file);
                    g_start = host::Counter();
                    g_enabled = 1;
                }
            }
        }
        return g_enabled == 1;
    }

    void MainStepStart(uint64_t iteration, bool tick, float alpha)
    {
        g_iteration = iteration;
        g_tick = tick;
        g_alpha = alpha;
        if (tick)
            g_ticks++;
    }

    void MainStepEnd()
    {
        if (++g_mainSteps % kRatePeriod == 0)
            LogRates();
    }

    void DisplayStart(uint32_t view)
    {
        g_displays++;
        g_view = view;
        g_rendered = 0;
        g_previousActors.swap(g_actors);
        g_actors.clear();
        g_bones.clear();
        g_lookAts.clear();
        g_inDisplay = true;
    }

    void DisplayEnd()
    {
        g_inDisplay = false;
        int64_t us = Microseconds();
        uint32_t cut = mem::Load32(jade::kCutCounter);
        uint32_t time = TimeStructure();

        WritePrefix(us, cut);
        std::fprintf(g_file, "F,,%u,%zu,%zu,%.6f,%u,%.6f,%.6f\n", g_rendered, g_actors.size(), g_bones.size(),
            time != 0 ? LoadFloat(time + jade::tim::kPeriod) : 0.0f,
            time != 0 ? mem::Load32(time + jade::tim::kFixedStep) : 0u,
            time != 0 ? LoadFloat(time + jade::tim::kDtScaled) : 0.0f,
            time != 0 ? LoadFloat(time + jade::tim::kDtRaw) : 0.0f);
        view_clock::Times times = view_clock::CurrentTimes();
        if (times.active)
        {
            // The view clock's image and tick game times, µs.
            WritePrefix(us, cut);
            std::fprintf(g_file, "T,,%llu,%llu,,,,,\n", (unsigned long long)times.image, (unsigned long long)times.tick);
        }
        const transforms::Block& block = transforms::LastBlock();
        if (block.active)
        {
            // The transforms: the image position written at, object matrices written and held at a
            // cut, object cuts at this tick, the camera (0 not written, 1 blended, 2 held) and its cut.
            WritePrefix(us, cut);
            std::fprintf(g_file, "I,,%.4f,%u,%u,%u,%u,%u,\n", block.alpha, block.written, block.held, block.cuts,
                block.camera, block.cameraCut);
            // The pose: copies written at alpha and held at a cut, bones written, copies with bones
            // left, skeleton cuts at this tick, copies of untracked objects.
            const pose::Block& poses = pose::LastBlock();
            WritePrefix(us, cut);
            std::fprintf(g_file, "P,,%u,%u,%u,%u,%u,%u,\n", poses.blended, poses.held, poses.bones, poses.partial,
                poses.cuts, poses.untracked);
        }
        if (g_view != 0)
        {
            Vector position = LoadVector(g_view + jade::view::kCameraPosition);
            Vector direction = LoadVector(g_view + jade::view::kCameraDirection);
            WritePrefix(us, cut);
            std::fprintf(g_file, "C,%08X,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n", g_view, position.x, position.y,
                position.z, direction.x, direction.y, direction.z, Finite(LoadFloat(g_view + jade::view::kFieldOfView)));
        }
        for (const Actor& actor : g_actors)
        {
            WritePrefix(us, cut);
            std::fprintf(g_file, "O,%08X,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,\n", actor.object, actor.position.x,
                actor.position.y, actor.position.z, actor.row.x, actor.row.y, actor.row.z);
        }
        for (const BoneRow& bone : g_bones)
        {
            WritePrefix(us, cut);
            std::fprintf(g_file, "B,%08X/%08X/%u,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f\n", bone.owner, bone.object,
                bone.bone, bone.world.x, bone.world.y, bone.world.z, bone.local.x, bone.local.y, bone.local.z,
                bone.distance);
        }
        for (const LookAt& look : g_lookAts)
        {
            // The icon as drawn: its position and the scale of its rows after the display; d the first
            // row's scale as the view block gave it.
            Vector position = LoadVector(look.matrix + jade::matrix::kTranslation);
            WritePrefix(us, cut);
            std::fprintf(g_file, "L,%08X,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f\n", look.object, position.x, position.y,
                position.z, RowScale(look.matrix, 0), RowScale(look.matrix, 1), RowScale(look.matrix, 2), look.scale);
        }
        if (g_displays % 60 == 0)
            std::fflush(g_file);
    }

    void NoteRendered(uint32_t object)
    {
        if (!g_inDisplay)
            return;
        g_rendered++;
        if (g_lookAts.size() < kMaxLookAts && HasModifier(object, kLookAtModifier))
        {
            uint32_t matrix = mem::Load32(object + jade::object::kMatrix);
            if (matrix != 0)
                g_lookAts.push_back({ object, matrix, RowScale(matrix, 0) });
        }
        if (g_actors.size() >= kMaxActors || !HasDynamics(object))
            return;
        for (const Actor& actor : g_actors)
            if (actor.object == object)
                return;
        // The matrix as the display draws it.
        uint32_t matrix = mem::Load32(object + jade::object::kMatrix);
        if (matrix != 0)
            g_actors.push_back({ object, LoadVector(matrix + jade::matrix::kTranslation), LoadVector(matrix) });
    }

    void NoteBone(uint32_t bone, uint32_t matrix)
    {
        if (!g_inDisplay || bone % kBoneStride != 0)
            return;
        uint32_t display = mem::Load32(jade::kDisplayData);
        uint32_t object = display != 0 ? mem::Load32(display + jade::view::kCurrentObject) : 0;
        uint32_t objectMatrix = object != 0 ? mem::Load32(object + jade::object::kMatrix) : 0;
        if (objectMatrix == 0)
            return;

        // The owner: the nearest actor within kOwnerDistance (the object itself when it is one),
        // among this display's actors so far and the previous display's.
        Vector at = LoadVector(objectMatrix + jade::matrix::kTranslation);
        uint32_t owner = 0;
        float distance = kOwnerDistance;
        for (const std::vector<Actor>* actors : { &g_actors, &g_previousActors })
            for (const Actor& actor : *actors)
            {
                float d = actor.object == object ? 0.0f
                    : std::hypot(at.x - actor.position.x, at.y - actor.position.y, at.z - actor.position.z);
                if (d <= distance)
                {
                    distance = d;
                    owner = actor.object;
                }
            }
        if (owner == 0)
            return;
        Vector local = LoadVector(matrix + jade::matrix::kTranslation);
        g_bones.push_back({ owner, object, bone, Transform(objectMatrix, local), local, distance });
    }
}
