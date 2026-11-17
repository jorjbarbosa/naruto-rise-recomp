// The title's destructions after the drawing of the handed packets, and their counters
// (destruction_order.h).

#include "game/destruction_order.h"

#include "game/addresses.h"
#include "game/interpolation/lifetime.h"
#include "game/render_packets.h"
#include "kernel/kernel.h"
#include "log.h"
#include "memory.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

DECLARE_ORIGINAL(JADE_END_OF_FRAME);
DECLARE_ORIGINAL(JADE_DEFERRED_DESTROY);
DECLARE_ORIGINAL(JADE_DISPLAY_DESTROYS);
DECLARE_ORIGINAL(JADE_SECTOR_SWITCH);
DECLARE_ORIGINAL(JADE_DESTROY_OBJECT);
DECLARE_ORIGINAL(JADE_DESTROY_OBJECT_CHECKED);
DECLARE_ORIGINAL(JADE_FREE);
DECLARE_ORIGINAL(JADE_HEAP_FREE);

namespace game::destruction
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // Above the slowest packets measured with the render thread saturated (about 1 s, many draws
        // under the interpolation's gate, step 7 document); a ceiling still, so a packet that never
        // finishes cannot hang the game. Timeouts are counted, the first kLoggedTimeouts per site logged.
        constexpr std::chrono::milliseconds kWaitLimit{ 2000 };
        constexpr uint32_t kReportTicks = 900; // end-of-frame calls between two reports (30 s at 30 ticks/s)
        constexpr uint32_t kLoggedTimeouts = 10;
        constexpr size_t kLoggedCallers = 16;

        enum Site
        {
            kDeferred,
            kDisplay,
            kSectorSwitch,
            kOther, // a destroy outside the three sites
            kSites
        };
        constexpr const char* kSiteNames[kSites] = { "deferred", "display", "sector switch", "other" };

        struct Counters
        {
            uint64_t calls = 0;   // with work; for the sector switch, those that reached a destroy or free
            uint64_t unread = 0; // a world packet was not read when the site was about to destroy
            uint64_t waits = 0, waitUs = 0, maxWaitUs = 0, timeouts = 0;
            uint64_t skipped = 0;      // not waited: an earlier wait for the same packets timed out
            uint64_t heldLock = 0;     // not waited: the thread held the device lock
            uint64_t renderThread = 0; // not waited: called on the render thread
            uint64_t destroys = 0, frees = 0, engineFrees = 0;
            uint64_t exposed = 0; // destroys and frees run with a world packet not read
        };

        // One site's call, on its thread.
        struct Scope
        {
            Site site;
            bool deferred = false; // wait at its first destroy or engine free instead of at its start
            bool checked = false;  // the wait (or the check that none is needed) happened
            bool exposed = false;  // it ran on with a world packet not read
            uint64_t destroys = 0, frees = 0, engineFrees = 0;
            // Heap frees before the deferred wait (realloc, a heap's virtual free: they bypass the engine's
            // free and cannot wait under the heap's lock), and those of them with a world packet not read.
            uint64_t uncheckedFrees = 0, exposedFrees = 0;
        };
        thread_local Scope* t_scope = nullptr;
        // The return address into the caller of JADE_DESTROY_OBJECT_CHECKED while it runs, else 0:
        // a destroy's own caller is then that function.
        thread_local uint32_t t_checkedCaller = 0;

        const bool g_waits = [] {
            const char* value = std::getenv("ROAN_DESTRUCTION_WAITS");
            return value == nullptr || std::string(value) != "0";
        }();

        std::mutex g_mutex; // everything below
        Counters g_counters[kSites]; // kOther's calls and destroys are g_otherDestroys
        std::map<uint32_t, uint64_t> g_unreadCallers; // kOther: guest return address -> destroys that found a packet
        uint32_t g_ticks = 0;
        std::string g_lastReport;
        // The handed count when a wait last timed out: no wait again until another packet is handed.
        uint64_t g_gaveUpAt = UINT64_MAX;

        std::atomic<uint64_t> g_otherDestroys{ 0 };
        std::atomic<uint64_t> g_sectorSwitches{ 0 };

        // The device lock records its owner by the title's thread id, which the kernel keeps as the
        // thread's id ([[r13+256]+332], 822DCE28).
        bool HoldsDeviceLock()
        {
            kernel::GuestThread* thread = kernel::CurrentThread();
            uint32_t lock = mem::Load32(jade::kDeviceLock);
            return thread != nullptr && lock != 0 && mem::Load32(lock + jade::device_lock::kOwner) == thread->id;
        }

        // A site is about to destroy with a world packet not read: waits until the packets handed so far
        // are read. True when it goes on with one still not read.
        bool WaitForDrawing(Site site)
        {
            bool heldLock = HoldsDeviceLock();
            bool renderThread = OnRenderThread();
            bool skipped;
            {
                std::lock_guard lock(g_mutex);
                skipped = g_gaveUpAt == PacketsHanded();
            }
            bool done = false;
            uint64_t waitedUs = 0;
            bool waited = false;
            if (g_waits && !heldLock && !renderThread && !skipped)
            {
                Clock::time_point start = Clock::now();
                done = WaitForPacketsRead(kWaitLimit);
                waitedUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
                waited = true;
            }

            std::lock_guard lock(g_mutex);
            Counters& c = g_counters[site];
            c.unread++;
            if (heldLock)
                c.heldLock++;
            else if (renderThread)
                c.renderThread++;
            else if (skipped && g_waits)
                c.skipped++;
            if (waited)
            {
                c.waits++;
                c.waitUs += waitedUs;
                c.maxWaitUs = std::max(c.maxWaitUs, waitedUs);
                if (!done)
                {
                    g_gaveUpAt = PacketsHanded();
                    if (++c.timeouts <= kLoggedTimeouts)
                        Log("destruction: %s: the world packet was not read within %lld ms (timeout %llu)",
                            kSiteNames[site], (long long)kWaitLimit.count(), (unsigned long long)c.timeouts);
                }
            }
            return !done;
        }

        // In a site that waits at its first destroy or engine free: that wait, once.
        void CheckDeferredWait(Scope& scope)
        {
            if (scope.checked)
                return;
            scope.checked = true;
            scope.exposed = WorldPacketPending() && WaitForDrawing(scope.site);
        }

        // A site's call: the wait when it has work (at its start, or at its first destroy or free), its
        // destroys and frees counted.
        template <class Original>
        void RunSite(Site site, bool work, bool deferredWait, Original original)
        {
            if (!work)
            {
                original();
                return;
            }
            Scope inner{ site };
            inner.deferred = deferredWait;
            if (!deferredWait)
            {
                inner.checked = true;
                inner.exposed = WorldPacketPending() && WaitForDrawing(site);
            }
            Scope* outer = t_scope;
            t_scope = &inner;
            original();
            t_scope = outer;

            // A site that waits at its first destroy or free counts only the calls that reached one.
            if (!inner.checked && inner.destroys == 0 && inner.frees == 0)
                return;
            std::lock_guard lock(g_mutex);
            Counters& c = g_counters[site];
            c.calls++;
            c.destroys += inner.destroys;
            c.frees += inner.frees;
            c.engineFrees += inner.engineFrees;
            c.exposed += inner.exposedFrees;
            if (inner.exposed)
                c.exposed += inner.destroys + inner.frees - inner.uncheckedFrees;
        }

        // A destroy outside the sites: waits like a site, and remembers its caller when a world packet
        // was not read.
        void OtherDestroy(uint32_t object, uint32_t caller)
        {
            g_otherDestroys.fetch_add(1, std::memory_order_relaxed);
            if (!WorldPacketPending())
                return;
            bool exposed = WaitForDrawing(kOther);

            std::lock_guard lock(g_mutex);
            if (exposed)
                g_counters[kOther].exposed++;
            auto [it, added] = g_unreadCallers.try_emplace(caller, 0);
            it->second++;
            if (added && g_unreadCallers.size() <= kLoggedCallers)
            {
                kernel::GuestThread* thread = kernel::CurrentThread();
                Log("destruction: object %08X destroyed outside the sites with a world packet not read (caller %08X, "
                    "thread %u%s%s)",
                    object, caller, thread != nullptr ? thread->id : 0, thread != nullptr && !thread->name.empty() ? " " : "",
                    thread != nullptr ? thread->name.c_str() : "");
            }
        }

        // Every kReportTicks end-of-frame calls, logs the counters when they changed. Main thread.
        void ReportPeriodically()
        {
            {
                std::lock_guard lock(g_mutex);
                if (++g_ticks % kReportTicks != 0)
                    return;
            }
            std::string summary = Summary();
            // The packet counts always move; report when anything after them changed.
            std::string counters = summary.substr(std::min(summary.find(';'), summary.size()));
            std::lock_guard lock(g_mutex);
            if (counters == g_lastReport)
                return;
            g_lastReport = counters;
            Log("destruction: %s", summary.c_str());
        }
    }

    std::string Summary()
    {
        std::lock_guard lock(g_mutex);
        char text[320];
        std::snprintf(text, sizeof(text), "waits %s, packets handed %llu read %llu", g_waits ? "on" : "off",
            (unsigned long long)PacketsHanded(), (unsigned long long)PacketsRead());
        std::string out = text;
        for (int i = 0; i < kSites; i++)
        {
            Counters c = g_counters[i];
            if (i == kOther)
                c.calls = c.destroys = g_otherDestroys.load(std::memory_order_relaxed);
            std::snprintf(text, sizeof(text),
                "; %s: calls %llu unread %llu waits %llu (%llu us, max %llu) timeouts %llu skipped %llu held lock %llu "
                "render thread %llu destroys %llu frees %llu (engine %llu) exposed %llu",
                kSiteNames[i], (unsigned long long)c.calls, (unsigned long long)c.unread,
                (unsigned long long)c.waits, (unsigned long long)c.waitUs, (unsigned long long)c.maxWaitUs,
                (unsigned long long)c.timeouts, (unsigned long long)c.skipped, (unsigned long long)c.heldLock,
                (unsigned long long)c.renderThread, (unsigned long long)c.destroys, (unsigned long long)c.frees,
                (unsigned long long)c.engineFrees, (unsigned long long)c.exposed);
            out += text;
        }
        for (auto& [caller, count] : g_unreadCallers)
        {
            std::snprintf(text, sizeof(text), " (%08X: %llu)", caller, (unsigned long long)count);
            out += text;
        }
        return out;
    }

    uint64_t SectorSwitches()
    {
        return g_sectorSwitches.load(std::memory_order_relaxed);
    }
}

namespace destruction = game::destruction;

// End of frame (8267DDC0, main step row 11, once per tick): the sector switch when the delivery did
// not run it, then the deferred destruction, both hooked on their own. Hooked as the report point.
PPC_FUNC(JADE_END_OF_FRAME)
{
    ORIGINAL(JADE_END_OF_FRAME)(ctx, base);
    destruction::ReportPeriodically();
}

// Deferred destruction (82659060, r3 the queue): releases, destroys and frees the queued entries under
// a critical section. Callers: the end of frame, the world streamer's thread (8265DC50) and a world
// unload path (826594D8). Hooked to wait before it, outside its critical section.
PPC_FUNC(JADE_DEFERRED_DESTROY)
{
    uint32_t queue = ctx.r3.u32;
    bool work = queue != 0 && mem::Load32(queue + 4) != 0;
    destruction::RunSite(destruction::kDeferred, work, false,
        [&] { ORIGINAL(JADE_DEFERRED_DESTROY)(ctx, base); });
}

// The display's queued destroys (82659F40, under the display, up to 128 per call, before the delivery
// hands the frame). Hooked to wait before it for the previous packet.
PPC_FUNC(JADE_DISPLAY_DESTROYS)
{
    bool work = mem::Load32(game::jade::kDisplayDestroyCount) != 0;
    destruction::RunSite(destruction::kDisplay, work, false,
        [&] { ORIGINAL(JADE_DISPLAY_DESTROYS)(ctx, base); });
}

// Sector switch (8267DA90, r3 the switch state): run by the delivery with its flag set, else by the
// end of frame, so once per tick. It destroys the objects of the sectors it leaves, but the sector
// manager's target (+44) stays set while a sector is active, so most calls destroy nothing. Hooked to
// wait at its first destroy or engine free, and to count its calls on the main thread.
PPC_FUNC(JADE_SECTOR_SWITCH)
{
    if (kernel::CurrentThread() == kernel::MainThread())
        destruction::g_sectorSwitches.fetch_add(1, std::memory_order_relaxed);
    uint32_t sectors = mem::Load32(game::jade::kSectors);
    bool work = sectors != 0 && mem::Load32(sectors + game::jade::kSectorsTarget) != 0;
    destruction::RunSite(destruction::kSectorSwitch, work, true,
        [&] { ORIGINAL(JADE_SECTOR_SWITCH)(ctx, base); });
}

// Destroy of a game object (8266BA98, r3 the object). Hooked to count it for its site, outside the
// sites to wait for a world packet before it, and to report it to the state interpolation's tracking.
PPC_FUNC(JADE_DESTROY_OBJECT)
{
    game::interpolation::lifetime::NoteDestroy(ctx.r3.u32);
    destruction::Scope* scope = destruction::t_scope;
    if (scope != nullptr)
    {
        if (scope->deferred)
            destruction::CheckDeferredWait(*scope);
        scope->destroys++;
    }
    else
    {
        uint32_t caller = destruction::t_checkedCaller != 0 ? destruction::t_checkedCaller : uint32_t(ctx.lr);
        destruction::OtherDestroy(ctx.r3.u32, caller);
    }
    ORIGINAL(JADE_DESTROY_OBJECT)(ctx, base);
}

// Destroy of a game object with a check first (8266C1F0, r3 the object): drops it from a list of the
// world change, then JADE_DESTROY_OBJECT. Its 12 callers are the usual way to destroy one object, from
// the logic and from the display (the lights' rebuild, 8258EEA8). Hooked so a destroy outside the
// sites names the caller of this function.
PPC_FUNC(JADE_DESTROY_OBJECT_CHECKED)
{
    uint32_t outer = destruction::t_checkedCaller;
    destruction::t_checkedCaller = uint32_t(ctx.lr);
    ORIGINAL(JADE_DESTROY_OBJECT_CHECKED)(ctx, base);
    destruction::t_checkedCaller = outer;
}

// The engine's free (820C0018): takes the memory manager's lock (82185028), then frees through its
// vtable +20. Hooked to count the frees that go through it inside a site, and to make a site that
// waits at its first destroy or free wait here, before the lock the render thread also takes.
PPC_FUNC(JADE_FREE)
{
    destruction::Scope* scope = destruction::t_scope;
    if (scope != nullptr)
    {
        if (scope->deferred)
            destruction::CheckDeferredWait(*scope);
        scope->engineFrees++;
    }
    ORIGINAL(JADE_FREE)(ctx, base);
}

// The heap's free (82188C88, r3 heap, r4 block): where the engine's free, realloc and the heaps'
// virtual free end, under the heap's lock. Hooked to count the frees inside a site, and to report the
// block to the state interpolation's tracking (its header still holds the size); never waits here, so
// a free that reaches it before a deferred wait (not through the engine's free) is only checked.
PPC_FUNC(JADE_HEAP_FREE)
{
    game::interpolation::lifetime::NoteFree(ctx.r4.u32);
    destruction::Scope* scope = destruction::t_scope;
    if (scope != nullptr && ctx.r4.u32 != 0)
    {
        scope->frees++;
        if (!scope->checked)
        {
            scope->uncheckedFrees++;
            if (game::WorldPacketPending())
                scope->exposedFrees++;
        }
    }
    ORIGINAL(JADE_HEAP_FREE)(ctx, base);
}
