// The host's bit of the engine's input flag (input_hold.h).

#include "game/input_hold.h"

#include "game/addresses.h"
#include "memory.h"

namespace game
{
    namespace
    {
        // Free in the game: a scan of every writer of the flag found only 4, 8 and 0x10, and the
        // game left this bit alone when the host set it (experiment 2).
        constexpr uint32_t kHostBit = 0x40000000;
    }

    void SetInputHeld(bool held)
    {
        uint32_t old = mem::AtomicLoad32(jade::kInputFlag);
        for (;;)
        {
            uint32_t desired = held ? (old | kHostBit) : (old & ~kHostBit);
            if (desired == old || mem::AtomicCas32(jade::kInputFlag, old, desired))
                return;
            old = mem::AtomicLoad32(jade::kInputFlag);
        }
    }
}
