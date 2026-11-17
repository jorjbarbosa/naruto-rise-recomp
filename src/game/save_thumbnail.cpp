// The save thumbnail: one capture per request.
//
// A save starts (82651F88) by allocating a thumbnail (20 bytes) and requesting its capture from the
// renderer's capture task (820ECD38): [task+60] = the thumbnail, [task+8] = 1 (pending). At every packet
// hand-off the main thread (820F01D8) sets each task's [+4] (enabled) from [+8], then calls its copy
// (vtable +44, here 8211D7B0: [+56] = [+60]). The render thread, drawing a packet, runs the capture
// (8211D878) when [+4] is set: it clears [+8] and [+60] on entry, draws, resolves, saves and compresses
// the image into the thumbnail at [+56], and sets the thumbnail's ready word [+16] last. Once it is set,
// the main thread hands the thumbnail to the save writer (82652210), which frees it when done
// (82583D28, then the free of the structure).
//
// The task's fields are one set while the packets are two: a hand-off that comes before the render
// thread reached the capture of the previous packet still finds [+8] set, enables the task again with
// the same thumbnail, and the next packet captures it once more. That second capture writes the
// thumbnail's buffers and fields after the writer took it, or after it was freed: the game's heap is
// corrupted (a free chunk handed out twice; seen as the renderer's light map turned into a loop, and the
// interface's worker calling address 0). The console's 30 Hz rarely lets the main thread overtake the
// render thread; above the game's rate (and with the view block every frame under the interpolation's
// gate, four hand-offs per tick at 120 Hz) it happens at almost every save.
//
// Here a request belongs to one capture. The task's copy takes a new request from the game's fields
// (clearing [+8] and [+60], as the capture would) and keeps it open: until it is captured, every hand-off
// enables the task with that thumbnail, whatever the render thread's lag. The capture runs only for the
// open request, and closes it by its generation (a thumbnail's address can come back for a later save);
// without one it returns the game's result for no thumbnail (0) and leaves the game's fields alone. A
// request made during a capture is put back after it (the capture clears the fields), unless the copy
// took one meanwhile. A thumbnail released (82583D28) before its capture closes its request, or withdraws
// it from the game's fields when no hand-off took it yet. So a thumbnail is captured exactly once, before
// the writer can see it ready.

#include "game/addresses.h"
#include "kernel/kernel.h"
#include "log.h"
#include "memory.h"

#include <cstdint>
#include <mutex>

DECLARE_ORIGINAL(JADE_CAPTURE_TASK_COPY);
DECLARE_ORIGINAL(JADE_THUMBNAIL_CAPTURE);
DECLARE_ORIGINAL(JADE_THUMBNAIL_RELEASE);

namespace
{
    namespace capture = game::jade::capture;

    // The requests, by generation. The task's copy runs on the main thread, the capture on the render
    // thread, the release on the main thread or the save writer's.
    std::mutex g_mutex;
    uint32_t g_task = 0;        // the capture task, once its copy ran
    uint64_t g_lastId = 0;      // the generation of the last request taken
    uint64_t g_taken = 0;       // requests taken from the game's fields (the capture's put-back checks it)
    uint32_t g_open = 0;        // the open request's thumbnail, 0 when none
    uint64_t g_openId = 0;      // ... and its generation
    uint32_t g_capturing = 0;   // the thumbnail of a capture in progress
    bool g_releasedInCapture = false;
    uint32_t g_saved = 0;       // a request in the game's fields when a capture started, to put back
    uint64_t g_captures = 0, g_skipped = 0, g_dropped = 0, g_replaced = 0;

    bool IsCaptureTask(uint32_t task)
    {
        return task != 0 && mem::Load32(task) == capture::kVtable;
    }
}

// The capture task's per-hand-off copy (main thread, inside 820F01D8, after [+4] = [+8]).
PPC_FUNC(JADE_CAPTURE_TASK_COPY)
{
    uint32_t task = ctx.r3.u32;
    if (!IsCaptureTask(task))
    {
        ORIGINAL(JADE_CAPTURE_TASK_COPY)(ctx, base);
        return;
    }
    std::lock_guard lock(g_mutex);
    g_task = task;
    uint32_t requested = mem::Load32(task + capture::kRequest);
    if (mem::Load32(task + capture::kPending) != 0 && requested != 0)
    {
        // A new request: taken from the game's fields, as the capture's entry would.
        if (g_open != 0 && g_open != requested)
        {
            g_replaced++;
            Log("save thumbnail: request %08X replaces %08X, not captured (as the game's request field)", requested,
                g_open);
        }
        g_open = requested;
        g_openId = ++g_lastId;
        g_taken++;
        mem::Store32(task + capture::kPending, 0);
        mem::Store32(task + capture::kRequest, 0);
        if (g_lastId <= 16)
            Log("save thumbnail: request %llu, thumbnail %08X", (unsigned long long)g_lastId, requested);
    }
    mem::Store32(task + capture::kThumbnail, g_open);
    mem::Store32(task + capture::kEnabled, g_open != 0 ? 1 : 0);
}

// The capture (render thread, from the dispatch 820F2470 when [+4] is set).
PPC_FUNC(JADE_THUMBNAIL_CAPTURE)
{
    uint32_t task = ctx.r3.u32;
    if (!IsCaptureTask(task))
    {
        ORIGINAL(JADE_THUMBNAIL_CAPTURE)(ctx, base);
        return;
    }
    uint32_t thumbnail = 0, pending = 0;
    uint64_t id = 0, taken = 0;
    {
        std::lock_guard lock(g_mutex);
        if (g_openId == 0)
        {
            // Enabled without an open request (its capture already ran, or a hand-off enabled it
            // between [+4] = [+8] and the copy): the game's result for no thumbnail.
            g_skipped++;
            ctx.r3.u64 = 0;
            return;
        }
        thumbnail = g_open;
        id = g_openId;
        g_capturing = thumbnail;
        g_releasedInCapture = false;
        taken = g_taken;
        pending = mem::Load32(task + capture::kPending);
        g_saved = mem::Load32(task + capture::kRequest);
        mem::Store32(task + capture::kThumbnail, thumbnail);
    }
    ORIGINAL(JADE_THUMBNAIL_CAPTURE)(ctx, base);

    std::lock_guard lock(g_mutex);
    if (pending != 0 && g_saved != 0 && g_taken == taken && mem::Load32(task + capture::kPending) == 0
        && mem::Load32(task + capture::kRequest) == 0)
    {
        mem::Store32(task + capture::kRequest, g_saved);
        mem::Store32(task + capture::kPending, pending);
    }
    g_saved = 0;
    g_capturing = 0;
    if (g_openId == id)
    {
        g_open = 0;
        g_openId = 0;
    }
    g_captures++;
    if (g_captures <= 16)
    {
        if (g_releasedInCapture)
            Log("save thumbnail: captured %08X (result %u), released as it finished", thumbnail, ctx.r3.u32);
        else
            Log("save thumbnail: captured %08X (result %u, ready %u)", thumbnail, ctx.r3.u32,
                mem::Load32(thumbnail + game::jade::thumbnail::kReady));
    }
}

// The release of a thumbnail's buffers (main thread or the save writer), before the structure's free.
PPC_FUNC(JADE_THUMBNAIL_RELEASE)
{
    uint32_t thumbnail = ctx.r3.u32;
    if (thumbnail != 0)
    {
        std::lock_guard lock(g_mutex);
        if (thumbnail == g_capturing)
        {
            // The writer takes it as soon as the capture's last store marks it ready; the capture's end
            // closes its request by generation.
            g_releasedInCapture = true;
        }
        else if (thumbnail == g_open)
        {
            g_open = 0;
            g_openId = 0;
            g_dropped++;
            Log("save thumbnail: %08X released before its capture, request dropped", thumbnail);
        }
        if (thumbnail == g_saved)
            g_saved = 0;
        // Still only in the game's fields (requested, no hand-off took it yet): withdrawn.
        if (g_task != 0 && mem::Load32(g_task + capture::kRequest) == thumbnail)
        {
            mem::Store32(g_task + capture::kRequest, 0);
            mem::Store32(g_task + capture::kPending, 0);
            g_dropped++;
            Log("save thumbnail: %08X released while requested, request withdrawn", thumbnail);
        }
    }
    ORIGINAL(JADE_THUMBNAIL_RELEASE)(ctx, base);
}
