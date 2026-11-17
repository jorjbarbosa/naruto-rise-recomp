#pragma once

// The lifetime of the guest memory the transforms write (state interpolation plan, invariant 6 and F2;
// step 4 document of the plan's folder).
//
// The transforms write object matrices found one tick or more earlier, by address. Every free ends in
// the heap's free (JADE_HEAP_FREE, step 1 document section 1), so its hook reports each freed block,
// with the extent read from its dlmalloc header, and the object destroy reports each destroyed object.
// Reports are kept only when they touch a watched page (64 KB pages marked by Watch, which only grow
// until Unwatch); the transforms take them (Drain) before they write and before they restore, and drop
// what they touch.
//
// The reports come from any thread (the world streamer and the loading thread free too); Watch and
// Drain are the main thread's.

#include <cstdint>
#include <vector>

namespace game::interpolation::lifetime
{
    // The extent watched and tested for a tracked object: a report touching its first word (its
    // destroy, or the free of its block) names it.
    constexpr uint32_t kObjectExtent = 4;

    // A guest address range, [begin, end).
    struct Range
    {
        uint32_t begin = 0, end = 0;
    };

    // Reports touching the pages of [begin, end) are kept from now on.
    void Watch(uint32_t begin, uint32_t end);

    // No page is watched any more; what was kept is dropped.
    void Unwatch();

    // A heap block is freed (r4 of JADE_HEAP_FREE, before the original runs: its header still holds
    // the size). Any thread.
    void NoteFree(uint32_t block);

    // A game object is destroyed (r3 of JADE_DESTROY_OBJECT). Any thread.
    void NoteDestroy(uint32_t object);

    // Takes the ranges reported since the last call. everything is set when reports were lost (too
    // many between two calls, or a freed block whose extent is unknown): the caller must drop
    // everything it tracks. Main thread.
    void Drain(std::vector<Range>& ranges, bool& everything);

    // Counters since the start: blocks whose header held no plausible size, and the drains that lost
    // reports (those blocks included).
    struct Totals
    {
        uint64_t badHeaders = 0, overflows = 0;
    };
    Totals Current();
}
