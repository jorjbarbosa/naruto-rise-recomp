// State the display writes from what it is given between ticks (display_writes.h).

#include "game/interpolation/display_writes.h"

#include "game/addresses.h"
#include "game/interpolation/diagnostics.h"
#include "game/interpolation/lifetime.h"
#include "game/interpolation/matrices.h"
#include "game/interpolation/transforms.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

DECLARE_ORIGINAL(JADE_GLOBAL_FROM_HIERARCHY);
DECLARE_ORIGINAL(JADE_LOCAL_FROM_GLOBAL);
DECLARE_ORIGINAL(JADE_SKELETON_CHILD);

namespace game::interpolation::display_writes
{
    namespace
    {
        using lifetime::kObjectExtent;

        // What the settling computes again for a hierarchy object.
        enum class Kind : uint8_t
        {
            kLocal,  // the display recomputed the local from a global it did not move
            kGlobal, // the display computed the global from the father
        };

        struct Note
        {
            uint32_t object = 0;
            Kind kind = Kind::kLocal;
            uint32_t depth = 0; // fathers above it (the settling's order)
        };

        // A call of the skeleton walk: its three arguments.
        struct Child
        {
            uint32_t child = 0, object = 0, bone = 0;
        };

        // The rows and the translation: the floats an Apply moves when it computes a global from the
        // father. The fourth column is left out: modifier 26 sets a scale there on a global it keeps.
        constexpr uint32_t kPlacementFloats = 0x7777;
        constexpr uint32_t kMaxDepth = 64; // a bound on a damaged hierarchy

        // Main thread state.
        int g_enabled = -1;
        bool g_active = false; // the current view block is under the gate with the transforms on
        // The hierarchy objects the display rendered in this view block, with their global as the view
        // block gave it (before their modifiers applied).
        std::unordered_map<uint32_t, matrices::Bits> g_rendered;
        std::unordered_map<uint32_t, size_t> g_noted; // object -> index in g_hierarchy
        std::vector<Note> g_hierarchy;
        std::vector<Child> g_children;
        bool g_inChild = false; // inside JADE_SKELETON_CHILD
        Totals g_totals;

        // The father of an object with a hierarchy, or 0.
        uint32_t Father(uint32_t object)
        {
            if ((mem::Load32(object + jade::object::kIdentity) & jade::object::kIdentityHierarchy) == 0)
                return 0;
            uint32_t base = mem::Load32(object + jade::object::kBase);
            uint32_t hierarchy = base != 0 ? mem::Load32(base + jade::base::kHierarchy) : 0;
            return hierarchy != 0 ? mem::Load32(hierarchy + jade::hierarchy::kFather) : 0;
        }

        // The hierarchy block of an object with a father, or 0.
        uint32_t Hierarchy(uint32_t object)
        {
            if (Father(object) == 0)
                return 0;
            return mem::Load32(mem::Load32(object + jade::object::kBase) + jade::base::kHierarchy);
        }

        uint32_t Depth(uint32_t object)
        {
            uint32_t depth = 0;
            for (uint32_t father = Father(object); father != 0 && depth < kMaxDepth; father = Father(father))
                depth++;
            return depth;
        }

        // A global computed by the display wins over a local recomputed from it.
        void AddNote(uint32_t object, Kind kind)
        {
            auto [it, added] = g_noted.try_emplace(object, g_hierarchy.size());
            if (added)
            {
                g_hierarchy.push_back({ object, kind, Depth(object) });
                lifetime::Watch(object, object + kObjectExtent);
            }
            else if (kind == Kind::kGlobal)
                g_hierarchy[it->second].kind = kind;
        }

        void Clear()
        {
            g_rendered.clear();
            g_noted.clear();
            g_hierarchy.clear();
            g_children.clear();
            g_inChild = false;
        }

        bool Freed(uint32_t begin, uint32_t end)
        {
            return transforms::Freed(begin, end);
        }

        // What a settling call changed in the 16 floats at matrix: the floats, and the largest
        // translation moved into the totals.
        uint32_t Count(const matrices::Bits& before, uint32_t matrix)
        {
            matrices::Bits after = matrices::LoadBits(matrix);
            uint32_t floats = 0;
            for (uint32_t i = 0; i < matrices::kFloats; i++)
                floats += before[i] != after[i];
            if (floats == 0)
                return 0;
            g_totals.floatsChanged += floats;
            float shift = matrices::Distance(matrices::Row(before.data(), 3), matrices::Row(after.data(), 3));
            g_totals.maxShift = std::max(g_totals.maxShift, double(shift));
            return floats;
        }
    }

    bool Enabled()
    {
        if (g_enabled < 0)
        {
            g_enabled = EnvironmentOff("ROAN_DISPLAY_WRITES") ? 0 : 1;
            if (g_enabled == 0)
                Log("display writes: off (ROAN_DISPLAY_WRITES=0)");
        }
        return g_enabled == 1;
    }

    void BlockStart(bool active)
    {
        Clear();
        g_active = active && Enabled();
    }

    void NoteRendered(uint32_t object)
    {
        if (!g_active || Hierarchy(object) == 0)
            return;
        uint32_t matrix = mem::Load32(object + jade::object::kMatrix);
        if (matrix != 0)
            g_rendered.try_emplace(object, matrices::LoadBits(matrix));
    }

    void LocalFromGlobal(uint32_t object, uint32_t caller)
    {
        // The engine does nothing for an object without a father; the skeleton walk is run again whole.
        if (!g_active || g_inChild || Hierarchy(object) == 0)
            return;
        auto rendered = g_rendered.find(object);
        uint32_t matrix = mem::Load32(object + jade::object::kMatrix);
        if (rendered == g_rendered.end() || matrix == 0)
        {
            g_totals.unknown++;
            for (uint32_t& slot : g_totals.unknownCallers)
            {
                if (slot == caller)
                    break;
                if (slot == 0)
                {
                    slot = caller;
                    Log("display writes: JADE_LOCAL_FROM_GLOBAL of %08X from %08X in the display, not settled", object,
                        caller);
                    break;
                }
            }
            return;
        }
        matrices::Bits now = matrices::LoadBits(matrix);
        bool moved = false;
        for (uint32_t i = 0; i < matrices::kFloats; i++)
            moved |= (kPlacementFloats & (1u << i)) != 0 && now[i] != rendered->second[i];
        AddNote(object, moved ? Kind::kGlobal : Kind::kLocal);
    }

    void GlobalFromHierarchy(uint32_t object, uint32_t out)
    {
        // Into a temporary (a father's chain for JADE_LOCAL_FROM_GLOBAL): nothing stays.
        if (!g_active || g_inChild || out == 0 || out != mem::Load32(object + jade::object::kMatrix)
            || Hierarchy(object) == 0)
            return;
        AddNote(object, Kind::kGlobal);
    }

    void SkeletonChildStart(uint32_t child, uint32_t object, uint32_t bone)
    {
        if (!g_active || child == 0 || object == 0 || bone == 0)
            return;
        g_children.push_back({ child, object, bone });
        lifetime::Watch(child, child + kObjectExtent);
        lifetime::Watch(object, object + kObjectExtent);
        lifetime::Watch(bone, bone + jade::matrix::kSize);
        g_inChild = true;
    }

    void SkeletonChildEnd()
    {
        g_inChild = false;
    }

    void Settle(PPCContext& ctx, uint8_t* base)
    {
        if (!g_active)
        {
            Clear();
            return;
        }
        g_active = false;
        g_totals.blocks++;
        // The caller of the delivery gets its registers back as the delivery left them.
        uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64;

        // Locals first, fathers before children: a local reads its father's chain of locals, and a
        // global through the hierarchy reads its fathers' locals.
        std::stable_sort(g_hierarchy.begin(), g_hierarchy.end(),
            [](const Note& a, const Note& b) { return a.depth < b.depth; });
        for (Kind pass : { Kind::kLocal, Kind::kGlobal })
        {
            for (const Note& note : g_hierarchy)
            {
                if (note.kind != pass)
                    continue;
                if (Freed(note.object, note.object + kObjectExtent))
                {
                    g_totals.freed++;
                    continue;
                }
                uint32_t hierarchy = Hierarchy(note.object);
                uint32_t matrix = mem::Load32(note.object + jade::object::kMatrix);
                if (hierarchy == 0 || matrix == 0)
                    continue;
                if (pass == Kind::kLocal)
                {
                    uint32_t local = hierarchy + jade::hierarchy::kLocal;
                    matrices::Bits before = matrices::LoadBits(local);
                    ctx.r3.u64 = note.object;
                    ORIGINAL(JADE_LOCAL_FROM_GLOBAL)(ctx, base);
                    g_totals.locals++;
                    g_totals.localsChanged += Count(before, local) != 0;
                }
                else
                {
                    matrices::Bits before = matrices::LoadBits(matrix);
                    ctx.r3.u64 = note.object;
                    ctx.r4.u64 = matrix;
                    ctx.r5.u64 = 1;
                    ORIGINAL(JADE_GLOBAL_FROM_HIERARCHY)(ctx, base);
                    g_totals.globals++;
                    g_totals.globalsChanged += Count(before, matrix) != 0;
                }
            }
        }

        // The skeleton walk again, last: its animated object may be a hierarchy object settled above.
        // It computes the bone from the child's local and the child's global from the bone and the
        // animated object's matrix (or, with kStatusGlobalToBone, the local from the global, then the
        // bone), as the console's display does with the tick's matrices.
        for (const Child& child : g_children)
        {
            if (Freed(child.child, child.child + kObjectExtent) || Freed(child.object, child.object + kObjectExtent)
                || Freed(child.bone, child.bone + jade::matrix::kSize))
            {
                g_totals.freed++;
                continue;
            }
            uint32_t matrix = mem::Load32(child.child + jade::object::kMatrix);
            if (matrix == 0 || mem::Load32(child.object + jade::object::kMatrix) == 0)
                continue;
            matrices::Bits global = matrices::LoadBits(matrix);
            matrices::Bits bone = matrices::LoadBits(child.bone);
            ctx.r3.u64 = child.child;
            ctx.r4.u64 = child.object;
            ctx.r5.u64 = child.bone;
            ORIGINAL(JADE_SKELETON_CHILD)(ctx, base);
            g_totals.children++;
            uint32_t changed = Count(global, matrix);
            changed += Count(bone, child.bone);
            g_totals.childrenChanged += changed != 0;
        }
        ctx.r3.u64 = r3;
        ctx.r4.u64 = r4;
        ctx.r5.u64 = r5;
        Clear();
    }

    Totals Totals::operator-(const Totals& start) const
    {
        Totals out = *this;
        out.blocks -= start.blocks;
        out.globals -= start.globals;
        out.locals -= start.locals;
        out.children -= start.children;
        out.globalsChanged -= start.globalsChanged;
        out.localsChanged -= start.localsChanged;
        out.childrenChanged -= start.childrenChanged;
        out.floatsChanged -= start.floatsChanged;
        out.freed -= start.freed;
        out.unknown -= start.unknown;
        return out;
    }

    Totals Current()
    {
        return g_totals;
    }
}
