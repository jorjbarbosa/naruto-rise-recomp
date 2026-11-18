#pragma once

// The host address space that holds guest memory: one reservation, committed as it is touched, whose
// writes the system records.

#include <cstdint>
#include <vector>

namespace host
{
    // The page size of the write record (TakeWrittenPages).
    constexpr uint32_t kWritePageSize = 0x1000;

    // Reserves size bytes (at most 4 GB) of read-write address space, zero until written. A page is
    // committed by the first access to it, from any thread, or by CommitAddressSpace; the system
    // records every write for TakeWrittenPages. Call once, before the other functions of this file.
    // Null when the space cannot be reserved or the commit on touch cannot be set up; the space is
    // never released.
    uint8_t* ReserveAddressSpace(uint64_t size);

    // Commits the pages of [offset, offset + size) of the reservation now (cut at its end). Host OS
    // calls (file reads and writes) fail on a page that is not committed instead of faulting, so call
    // this before handing them a range. A failure is ignored. Any thread.
    void CommitAddressSpace(uint32_t offset, uint32_t size);

    // Appends the offset of every page of [offset, offset + size) of the reservation (cut at its end)
    // written since the last call that covered it, and resets those pages. Every write counts: guest
    // code, host code and the host OS. Any thread, but a page is reported to one caller only. False
    // when the system refuses the query (logged once); nothing is appended.
    bool TakeWrittenPages(uint32_t offset, uint32_t size, std::vector<uint32_t>& pages);
}
