#pragma once

// Calls of guest functions from a hook, on the guest stack below the hook caller's frame.

#include <ppc_context.h>

#include <cstdint>

namespace game
{
    // A context for the calls: a copy of the hook's, whose stack starts gap bytes below the caller's
    // frame, with a scratch area scratch bytes above that, for arguments and results the calls read
    // and write (the callees' frames grow down from the start). gap must be larger than scratch plus
    // what is kept there. Destroying it hands the host's floating-point mode the calls left back to the
    // hook's context, which caches it (PPCFPSCRRegister::csr): the guest code after the hook relies on
    // the cache matching the host. The calling thread only.
    class GuestCall
    {
    public:
        GuestCall(PPCContext& ctx, uint32_t gap, uint32_t scratch) : ctx_(ctx), call_(ctx)
        {
            call_.r1.u64 = (ctx.r1.u32 - gap) & ~15u;
            scratch_ = call_.r1.u32 + scratch;
        }
        ~GuestCall() { ctx_.fpscr = call_.fpscr; }

        GuestCall(const GuestCall&) = delete;
        GuestCall& operator=(const GuestCall&) = delete;

        PPCContext& Context() { return call_; }
        // The guest address of the scratch area.
        uint32_t Scratch() const { return scratch_; }

    private:
        PPCContext& ctx_;
        PPCContext call_;
        uint32_t scratch_ = 0;
    };
}
