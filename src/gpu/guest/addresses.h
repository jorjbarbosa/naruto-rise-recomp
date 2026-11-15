#pragma once

// Guest addresses and D3D::CDevice offsets of the XDK 6274 D3D library in default.xex, named in one
// place. Names come from the XDK library match (EngineSrc build/xenonmatch) where there is one.

#include <cstdint>

// Hooked functions: the symbols of the recompiled functions. A definition with PPC_FUNC(<name>)
// replaces every call to them (they are weak aliases of their __imp__ implementation).
#define D3D_INITIALIZE_RING sub_8212FA80          // D3D::CDevice ring setup, inside device creation
#define D3D_INITIALIZE_XPS sub_821480D0           // D3D::InitializeXps
#define D3D_KICK_OFF sub_8212F608                 // D3D::CDevice::KickOff
#define D3D_START_NEW_SEGMENT sub_8212EBB0        // D3D::CDevice::StartNewSegment
#define D3D_ALLOCATE_FROM_RING sub_8212EA18       // ring space after the segment (BeginVertices...)
#define D3D_BLOCK_ON_FENCE sub_8212F708           // D3DDevice_BlockOnFence
#define D3D_ADD_COMMANDS_TO_PRIMARY sub_8212EE38  // D3D::CDevice::AddCommandsToPrimaryBuffer
#define D3D_ADD_CALLS_TO_PRIMARY sub_8212EF28     // D3D::CDevice::AddCallsToPrimaryBuffer
#define D3D_QUEUE_INDIRECT_BUFFER sub_8212F530    // D3D::CDevice::QueueIndirectBuffer
#define D3D_CLEAR sub_82131018                    // D3DDevice_ClearF
#define D3D_BEGIN_TILING sub_8213A7E0             // D3DDevice_BeginTiling (LTCG: fixed tile table)
#define D3D_END_TILING sub_8213AC80               // D3DDevice_EndTiling
#define D3D_RESOLVE sub_821398C8                  // D3DDevice_Resolve
#define D3D_DESTROY_RESOURCE sub_8212D750         // D3D::DestroyResource
#define D3D_UNLOCK_RESOURCE sub_8212DBE0          // D3D::UnlockResource
#define D3D_LOCK_SURFACE sub_8212C7E8             // D3D::LockSurface (every texture and surface lock)
#define D3D_QUERY_GET_DATA sub_8213B7F8           // D3DQuery_GetData
#define D3D_DRAW_INDEXED_VERTICES sub_82137E00    // D3DDevice_DrawIndexedVertices
#define D3D_DRAW_VERTICES sub_82137A18            // D3DDevice_DrawVertices
#define D3D_BEGIN_VERTICES sub_821375A0           // D3DDevice_BeginVertices (LTCG: fan, 4 vertices of 20 bytes)
#define D3D_PATCH_VERTEX_SHADER sub_82140540      // D3D::PatchVertexShaderToMatchVertexDeclaration
#define JADE_CREATE_VERTEX_SHADER sub_820F5150    // Jade shader database: builds a D3D vertex shader object
#define JADE_CREATE_PIXEL_SHADER sub_820F5348     // Jade shader database: builds a D3D pixel shader object

// The recompiled implementation of a hooked function, to run the original from its hook:
// DECLARE_ORIGINAL(D3D_RESOLVE); ... ORIGINAL(D3D_RESOLVE)(ctx, base);
#define ORIGINAL(name) ORIGINAL_EXPANDED(name)
#define ORIGINAL_EXPANDED(name) __imp__##name
#define DECLARE_ORIGINAL(name) extern "C" PPC_FUNC(ORIGINAL(name))

namespace gpu::guest
{
    // D3D::CDevice fields (byte offsets from the device).
    namespace dev
    {
        // Segment of the command ring being written. +48 points at the last dword written (packets
        // go at +48 + 4); a writer that finds +48 above the soft limit +56 calls KickOff first.
        // The soft limit sits 160 bytes below the segment end +52.
        constexpr uint32_t kCursor = 48;
        constexpr uint32_t kSegmentEnd = 52;
        constexpr uint32_t kSegmentLimit = 56;
        constexpr uint32_t kSegmentMargin = 160;

        // Writeback block (96 bytes): +0 is the last completed fence, written by the GPU.
        constexpr uint32_t kWriteback = 10896;
        // Scratch block (32 bytes) that the library's GPU packets address.
        constexpr uint32_t kScratch = 10900;
        // Fence of the open batch. KickOff closes the batch with it and adds 2.
        constexpr uint32_t kNextFence = 10908;
        // Initial value of kNextFence (the ring setup stores it when the field is 0).
        constexpr uint32_t kFirstFence = 3;

        // Shadows of the GPU state that the draw commits send: texture and vertex fetch constants (32
        // slots of 6 dwords), then the vertex and pixel shader float constants (256 float4 each).
        constexpr uint32_t kFetchConstants = 0x480;
        constexpr uint32_t kVertexConstants = 0x780;
        constexpr uint32_t kPixelConstants = 0x1780;
        constexpr uint32_t kConstantBytes = 0x1000;

        // Boolean shader constants: vertex shader b0..b31, pixel shader b0..b31.
        constexpr uint32_t kVertexBooleans = 10112;
        constexpr uint32_t kPixelBooleans = 10128;

        // The user clip planes D3DDevice_SetClipPlane keeps, PA_CL_UCP_0..5 in clip space: 4 floats
        // each. A plane applies when its bit of PA_CL_CLIP_CNTL is set.
        constexpr uint32_t kClipPlanes = 10272;

        // Register blocks: register base + i at offset + 4 i.
        constexpr uint32_t kRegisters2000 = 10368; // 0x2000..0x200F, then 0x2080..0x2082 (window offset, scissor)
        constexpr uint32_t kRegisters2100 = 10444; // 0x2100..0x2114
        constexpr uint32_t kRegisters2200 = 10548; // 0x2200..0x220B
        constexpr uint32_t kRegisters2300 = 10680; // 0x2300..0x2307
        constexpr uint32_t kRegisters2380 = 10832; // 0x2380..0x2383 (D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_DEPTHBIAS)

        // Flags byte. Bit 0x80: a command buffer is being recorded, and kicks are not submitted.
        constexpr uint32_t kFlags10940 = 10940;
        constexpr uint8_t kFlagRecording = 0x80;

        // The bound vertex declaration (D3DVertexDeclaration).
        constexpr uint32_t kVertexDeclaration = 11812;

        // Nonzero when scissoring is on (D3DRS_SCISSORTESTENABLE).
        constexpr uint32_t kScissorEnable = 11848;

        // The bound index buffer (D3DIndexBuffer).
        constexpr uint32_t kIndexBuffer = 12428;

        // Bound surface headers: render targets 0 to 3, then the depth-stencil surface.
        constexpr uint32_t kRenderTargets = 12432;
        constexpr uint32_t kRenderTargetCount = 4;
        constexpr uint32_t kDepthStencil = 12448;

        // Stride of each vertex stream, in dwords (one byte per stream).
        constexpr uint32_t kStreamStrides = 12520;

        // The bound pixel and vertex shader objects.
        constexpr uint32_t kPixelShader = 12684;
        constexpr uint32_t kVertexShader = 12688;

        // Viewport: x, y, width, height (floats).
        constexpr uint32_t kViewport = 12640;
        // Scissor rectangle: left, top, right, bottom.
        constexpr uint32_t kScissor = 12668;

        // Extent of the tiled area of the pass between BeginTiling and EndTiling (the largest right
        // and bottom of its tiles).
        constexpr uint32_t kTilingWidth = 13172;
        constexpr uint32_t kTilingHeight = 13176;

        // Nonzero while kicks are held back: KickOff then queues the segment in a list at +13352
        // instead of submitting it, and BlockOnFence returns at once for the open batch.
        constexpr uint32_t kKickHold = 13216;

        // Presentation interval of the frames, in 60 Hz vblanks (D3DPRESENT_INTERVAL_*).
        constexpr uint32_t kPresentInterval = 13580;

        // Fence of the previous frame, set by D3DDevice_Swap; StartNewSegment waits for it.
        constexpr uint32_t kFrameFence = 14924;

        // The gamma ramp D3DDevice_SetGammaRamp keeps for the display: D3DGAMMARAMP, 768 16-bit
        // words.
        constexpr uint32_t kGammaRamp = 14988;

        // Changes the next Swap applies. Bit 1: the gamma ramp at kGammaRamp was set.
        constexpr uint32_t kSwapPending = 16692;
        constexpr uint32_t kSwapPendingGamma = 2;

        // Creation flags. Bit 0: StartNewSegment does not wait for kFrameFence.
        constexpr uint32_t kCreateFlags = 22280;
        constexpr uint32_t kCreateNoSegmentWait = 1;
    }

    // Resource headers (D3DResource and the structures that extend it).
    namespace res
    {
        // Common word. Bits 0..3: the resource type.
        constexpr uint32_t kCommon = 0;
        constexpr uint32_t kTypeMask = 0xF;
        constexpr uint32_t kTypeVertexBuffer = 1;
        constexpr uint32_t kTypeTexture = 3;
        constexpr uint32_t kTypeSurface = 4;
        // Bits 8..11: the lock count, which UnlockResource decrements.
        constexpr uint32_t kLockCountShift = 8;
        constexpr uint32_t kLockCountMask = 0xF;

        // D3DSurface (48 bytes). Multisampling: halfword, bits 0..1 (0: none, 1: 2 samples, 2: 4).
        constexpr uint32_t kSurfaceSamples = 0x18;
        // Size: width - 1 in bits 18..31, height - 1 in bits 3..17.
        constexpr uint32_t kSurfaceSize = 0x24;
        // D3DFORMAT. Bits 0..5: the texture format of the surface (GPUTEXTUREFORMAT).
        constexpr uint32_t kSurfaceFormat = 0x28;

        // D3DBaseTexture: the texture fetch constant (6 dwords) of the texture.
        constexpr uint32_t kTextureFetch = 0x1C;

        // D3DVertexBuffer: the vertex fetch constant (2 dwords, DecodeVertexFetch), with the buffer's guest
        // address.
        constexpr uint32_t kVertexBufferFetch = 0x18;

        // D3DIndexBuffer. Common word: bit 31 set for 32-bit indices, bits 29..30 the endian swap.
        constexpr uint32_t kIndex32 = 0x80000000;
        constexpr uint32_t kIndexEndianShift = 29;
        // Address of the index data.
        constexpr uint32_t kIndexData = 0x18;

        // D3DVertexDeclaration: an id the library gives each declaration at its first patch (0
        // before).
        constexpr uint32_t kDeclarationId = 48;
    }

    // D3DQuery.
    namespace query
    {
        constexpr uint32_t kType = 4; // D3DQUERYTYPE: 8 event, 9 occlusion, 10 screen extent
        constexpr uint32_t kTypeOcclusion = 9;
    }

    // D3D shader objects as the Jade shader database builds them. Each holds a copy of the virtual
    // part of its shader container and the address of the physical part (the microcode and the
    // literal constants), as laid out in the shader database.
    namespace shader
    {
        constexpr uint32_t kVertexContainer = 0x368;
        constexpr uint32_t kVertexPhysical = 0x20;
        // Container flags of a vertex shader. 0x20: draws without a pixel shader use a variant of
        // the shader; 0x40: the library does not patch its vertex fetches.
        constexpr uint32_t kFlagDepthOnlyVariant = 0x20;
        constexpr uint32_t kFlagUnpatched = 0x40;
        constexpr uint32_t kPixelContainer = 0x28;
        constexpr uint32_t kPixelPhysical = 0x18;

        // Shader container fields.
        constexpr uint32_t kVirtualSize = 0x04;
        constexpr uint32_t kPhysicalSize = 0x08;
        constexpr uint32_t kShaderOffset = 0x18; // the shader header, from the container

        // Shader header: offset and size of the microcode in the physical part.
        constexpr uint32_t kCodeOffset = 0x00;
        constexpr uint32_t kCodeSize = 0x04;
        // Vertex shader header: the vertex elements (one dword each: fetch address in bits 0..11,
        // usage in bits 12..15, usage index in bits 16..19) start at kElements + 4 * kElementBase.
        constexpr uint32_t kElementBase = 0x18;
        constexpr uint32_t kElementCount = 0x1C;
        constexpr uint32_t kElements = 0x24;
    }

    // GPU registers, by number, in the register blocks of the device (dev::kRegisters*).
    namespace reg
    {
        constexpr uint32_t kDepthInfo = 0x2002;        // RB_DEPTH_INFO
        constexpr uint32_t kResetIndex = 0x2103;       // VGT_MULTI_PRIM_IB_RESET_INDX
        constexpr uint32_t kColorMask = 0x2104;        // RB_COLOR_MASK
        constexpr uint32_t kBlendColor = 0x2105;       // RB_BLEND_RED..ALPHA (4 floats)
        constexpr uint32_t kStencilRefMaskBack = 0x210C; // RB_STENCILREFMASK_BF
        constexpr uint32_t kStencilRefMask = 0x210D;   // RB_STENCILREFMASK
        constexpr uint32_t kAlphaRef = 0x210E;         // RB_ALPHA_REF (float)
        constexpr uint32_t kViewportXScale = 0x210F;   // PA_CL_VPORT_XSCALE..ZOFFSET (6 floats)
        constexpr uint32_t kDepthControl = 0x2200;     // RB_DEPTHCONTROL
        constexpr uint32_t kBlendControl = 0x2201;     // RB_BLENDCONTROL0
        constexpr uint32_t kColorControl = 0x2202;     // RB_COLORCONTROL
        constexpr uint32_t kClipControl = 0x2204;      // PA_CL_CLIP_CNTL
        constexpr uint32_t kRasterControl = 0x2205;    // PA_SU_SC_MODE_CNTL
        constexpr uint32_t kViewportControl = 0x2206;  // PA_CL_VTE_CNTL
        constexpr uint32_t kEdramMode = 0x2208;        // RB_MODECONTROL
        constexpr uint32_t kVertexControl = 0x2302;    // PA_SU_VTX_CNTL
        constexpr uint32_t kPolyOffsetFront = 0x2380;  // PA_SU_POLY_OFFSET_FRONT_SCALE, _OFFSET (floats)
        constexpr uint32_t kPolyOffsetBack = 0x2382;   // PA_SU_POLY_OFFSET_BACK_SCALE, _OFFSET (floats)
    }

    // GPUTEXTUREFORMAT values of the surfaces and resolve destinations the renderer covers.
    namespace format
    {
        constexpr uint32_t k8 = 2;
        constexpr uint32_t k5_6_5 = 4;
        constexpr uint32_t k8_8_8_8 = 6;
        constexpr uint32_t k24_8 = 22;
        constexpr uint32_t k24_8Float = 23;
    }
}
