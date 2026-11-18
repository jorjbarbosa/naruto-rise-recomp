// The game's own options through its option store (game_options.h).

#include "game/game_options.h"

#include "game/addresses.h"
#include "loader.h"
#include "log.h"
#include "memory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

DECLARE_ORIGINAL(AI_OPTIONS_LOAD);
PPC_EXTERN_FUNC(AI_OPTION_GET);
PPC_EXTERN_FUNC(AI_OPTION_SET);
PPC_EXTERN_FUNC(AI_OPTIONS_SAVE);
PPC_EXTERN_FUNC(AI_OPTION_APPLY);

namespace game::options
{
    namespace
    {
        // The guest stack the procedures run on: this far below the main step's frame, with a scratch
        // area for their results above their own frames.
        constexpr uint32_t kStackGap = 1024;
        constexpr uint32_t kScratch = 512;
        // XN_SYS_UI 0 puts back the vibration switch the game saved when it paused (game-side-findings.md
        // section 7): a vibration set while paused is applied again this many steps after the resume.
        constexpr int kResumeSteps = 3;

        // The store's instance, as the game passes it to its load (thread of the AI scripts).
        std::atomic<uint32_t> g_store{ 0 };
        std::atomic<bool> g_loaded{ false };
        std::array<std::atomic<int32_t>, kShown> g_values{};
        // Per option, the writes queued and not yet made: a publish leaves those values alone.
        std::array<std::atomic<int>, kShown> g_pending{};
        std::atomic<bool> g_watching{ false };
        std::atomic<bool> g_published{ false }; // the values were published since Watch(true)
        std::atomic<bool> g_japaneseOffered{ false };
        std::atomic<bool> g_askJapanese{ false }; // ask once per Watch(true)

        std::mutex g_queueMutex;
        std::vector<std::pair<int, int32_t>> g_queue;
        std::vector<std::pair<int, int>> g_applyLater; // (index, steps left), thread of the AI scripts
        std::atomic<bool> g_saveRequested{ false };
        std::atomic<uint32_t> g_saves{ 0 }; // saves made (or tried)
        std::atomic<bool> g_reapplyVibration{ false };
        bool g_changed = false;           // host main thread
        bool g_vibrationSet = false;      // host main thread: since Watch(true)

        // A context for calling the procedures from the main step: the caller's registers, a stack
        // below its frame.
        PPCContext CallContext(const PPCContext& ctx, uint32_t& scratch)
        {
            PPCContext call = ctx;
            call.r1.u64 = (ctx.r1.u32 - kStackGap) & ~15u;
            scratch = call.r1.u32 + kScratch;
            return call;
        }
    }

    void Watch(bool watching)
    {
        if (watching)
        {
            g_published.store(false, std::memory_order_release);
            g_askJapanese.store(true, std::memory_order_release);
            g_vibrationSet = false;
        }
        g_watching.store(watching, std::memory_order_release);
    }

    bool Loaded()
    {
        return g_loaded.load(std::memory_order_acquire);
    }

    bool Ready()
    {
        return Loaded() && g_published.load(std::memory_order_acquire);
    }

    int32_t Value(int index)
    {
        return index >= 0 && index < kShown ? g_values[index].load(std::memory_order_relaxed) : 0;
    }

    void Set(int index, int32_t value)
    {
        if (index < 0 || index >= kShown || !Ready())
            return;
        g_values[index].store(value, std::memory_order_relaxed);
        g_pending[index].fetch_add(1, std::memory_order_acq_rel);
        g_changed = true;
        if (index == kVibration)
            g_vibrationSet = true;
        std::lock_guard lock(g_queueMutex);
        g_queue.emplace_back(index, value);
    }

    bool Changed()
    {
        return g_changed;
    }

    void RequestSave()
    {
        g_changed = false;
        g_saveRequested.store(true, std::memory_order_release);
    }

    bool SaveBeforeQuit(double seconds)
    {
        if (!g_changed || !Loaded())
            return true;
        uint32_t saves = g_saves.load(std::memory_order_acquire);
        RequestSave();
        auto limit = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (g_saves.load(std::memory_order_acquire) == saves)
        {
            if (std::chrono::steady_clock::now() >= limit)
            {
                Log("game options: warning: not saved before quitting (the game did not step)");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

    void ReapplyAfterResume()
    {
        if (g_vibrationSet)
            g_reapplyVibration.store(true, std::memory_order_release);
    }

    bool JapaneseVoicesOffered()
    {
        return g_japaneseOffered.load(std::memory_order_relaxed);
    }

    void MainStep(PPCContext& ctx, uint8_t* base)
    {
        uint32_t store = g_store.load(std::memory_order_acquire);
        if (store == 0 || !g_loaded.load(std::memory_order_acquire))
            return;
        std::vector<std::pair<int, int32_t>> queue;
        {
            std::lock_guard lock(g_queueMutex);
            queue.swap(g_queue);
        }
        if (g_reapplyVibration.exchange(false, std::memory_order_acq_rel))
            g_applyLater.emplace_back(kVibration, kResumeSteps);
        bool save = g_saveRequested.exchange(false, std::memory_order_acquire);
        bool watching = g_watching.load(std::memory_order_acquire);
        if (queue.empty() && g_applyLater.empty() && !save && !watching)
            return;

        uint32_t scratch = 0;
        PPCContext call = CallContext(ctx, scratch);
        // fn_Option_SetValue (store, int* changed, index, value): clamps, stores and applies.
        for (auto [index, value] : queue)
        {
            call.r3.u64 = store;
            call.r4.u64 = scratch;
            call.r5.u64 = uint32_t(index);
            call.r6.u64 = uint32_t(value);
            AI_OPTION_SET(call, base);
            g_pending[index].fetch_sub(1, std::memory_order_acq_rel);
        }
        // fn_Option_Apply (store, index): the value as it is, applied again.
        for (auto it = g_applyLater.begin(); it != g_applyLater.end();)
        {
            if (--it->second > 0)
            {
                ++it;
                continue;
            }
            call.r3.u64 = store;
            call.r4.u64 = uint32_t(it->first);
            AI_OPTION_APPLY(call, base);
            it = g_applyLater.erase(it);
        }
        if (save)
        {
            call.r3.u64 = store;
            call.r4.u64 = scratch;
            mem::Store32(scratch, 0);
            AI_OPTIONS_SAVE(call, base);
            uint32_t ok = mem::Load32(scratch);
            Log("game options: %s", ok != 0 ? "saved to the profile" : "not saved (the profile is not ready)");
            g_saves.fetch_add(1, std::memory_order_acq_rel);
        }
        if (watching)
        {
            // fni_Option_GetValue (store, int* out, index); a value with a write still queued keeps
            // the one the menu set.
            for (int index = 0; index < kShown; index++)
            {
                call.r3.u64 = store;
                call.r4.u64 = scratch;
                call.r5.u64 = uint32_t(index);
                AI_OPTION_GET(call, base);
                if (g_pending[index].load(std::memory_order_acquire) == 0)
                    g_values[index].store(int32_t(mem::Load32(scratch)), std::memory_order_relaxed);
            }
            // What the Options track asks once before it shows the Japanese voices' row.
            if (g_askJapanese.exchange(false, std::memory_order_acq_rel))
                if (PPCFunc* offered = FindGuestFunction(mem::Load32(ai::kJapaneseVoicesNative)))
                {
                    call.r3.u64 = 1;
                    offered(call, base);
                    g_japaneseOffered.store(call.r3.u32 != 0, std::memory_order_relaxed);
                }
            g_published.store(true, std::memory_order_release);
        }
    }
}

using namespace game;

// The store's load (the Press Start track, and again on a profile change): its instance is recorded,
// and its values are the game's from then on.
PPC_FUNC(AI_OPTIONS_LOAD)
{
    uint32_t store = ctx.r3.u32;
    ORIGINAL(AI_OPTIONS_LOAD)(ctx, base);
    options::g_store.store(store, std::memory_order_release);
    if (!options::g_loaded.exchange(true, std::memory_order_acq_rel))
        Log("game options: the store is loaded (%08X)", store);
}
