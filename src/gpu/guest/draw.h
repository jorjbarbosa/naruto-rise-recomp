#pragma once

// Draws of the guest D3D device.

namespace gpu::guest
{
    // D3DDevice_BeginVertices returns space for the vertices of a quad, which the title writes after
    // the call. The quad is posted before the next operation the title's thread posts to the
    // renderer: the renderer calls this before queueing anything (Renderer::BeforePost). A call from
    // another thread does nothing, since the title may still be writing the vertices.
    void FlushInlineDraw();
}
