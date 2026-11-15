// Hooks on the device creation (8213ECE8). The creation runs as the original: its CPU setup and the
// packets it writes (into the discard segment) need no replacement. Only the parts that build the
// command ring and its worker threads are replaced.

#include "gpu/guest/addresses.h"
#include "gpu/guest/device.h"
#include "kernel/kernel.h"

// Ring setup (device, ring parameters): allocates the ring, the writeback and scratch blocks and
// writes the first packets; returns an HRESULT. UninitializeDevice calls it with no parameters to
// free the ring.
PPC_FUNC(D3D_INITIALIZE_RING)
{
    if (ctx.r4.u32 != 0)
        gpu::guest::GetDevice().InitializeArena(ctx.r3.u32);
    ctx.r3.u64 = 0;
}

// D3D::InitializeXps(device): starts the XPS worker threads, which feed the ring. Reported as done
// with no workers, as the original does when the device is created without XPS.
PPC_FUNC(D3D_INITIALIZE_XPS)
{
    ctx.r3.u64 = 1;
}
