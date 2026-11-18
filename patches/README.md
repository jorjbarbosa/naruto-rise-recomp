# Submodule patches

Our changes to the submodules (`tools/` and `thirdparty/`), kept as patches instead of a fork. Each folder is
named after a submodule (the last part of its path) and holds `.patch` files (output of `git diff` inside the
submodule), applied in alphabetical order.

## Usage

```powershell
git submodule update --init --recursive
./patches/apply.ps1            # apply; safe to rerun, already applied patches are skipped
./patches/apply.ps1 -Revert    # undo, in reverse order
```

On Linux, `./patches/apply.sh` and `./patches/apply.sh --revert` do the same.

Patches are applied in order, so the script takes the last patch of a folder that reverses cleanly as applied
together with every patch before it (an earlier patch checked on its own can fail to reverse when a later one
changed the lines around it).

After applying, `git status` in the project shows the patched submodules as `(modified content)`. That is
expected: each submodule stays on its upstream commit and the changes live only here.

### Updating the submodule

1. `./patches/apply.ps1 -Revert`
2. Update the submodule (`git -C tools/XenonRecomp pull`, then `git add tools/XenonRecomp`).
3. `./patches/apply.ps1`. If a patch no longer applies, fix the files by hand and regenerate it.

### Creating or editing a patch

With the previous patches applied and staged in the submodule (`git -C tools/XenonRecomp add -A`), make the
changes and run `git -C tools/XenonRecomp diff --output=../../patches/XenonRecomp/000N-name.patch`. Then
`git -C tools/XenonRecomp reset` unstages everything. Patches are kept in LF (`.gitattributes`).

## XenonRecomp

Findings from the analysis of the `default.xex` and `AI2C.dll` of Naruto: Rise of a Ninja (XDK 6274).

### 0001: function analysis with jump tables and tail calls

`Function::Analyze` (XenonAnalyse) takes optional hints (`FunctionAnalyzeHints`), built by
`Recompiler::Analyse` before the sweep:

- **Jump tables from the TOML.** Before, a `bctr` ended the function and every `case` became a `return` in the
  generated code. Now the table labels become blocks of the function, including when the table sits in the
  middle of the code, between the `bctr` and the cases.
  - In Naruto there were 58 leaf functions (no `.pdata`) and 992 "Switch case ... outside function" warnings.
  - They include engine code: the AI natives `AI_OBJ_PagGetf`/`PagSetf`, `MDF_Apply_58` (vegetation),
    `GEO_Render_LightVertices`, `TEX_l_File_Read` and the DARE sound engine.
- **Known function entry points** (`.pdata`, `bl` targets, functions from the TOML, register save/restore
  helpers). A `b` or `bc` to one of them is now a tail call, and the analyzer no longer follows it.
  - Example in Naruto: the thunk at `0x8216ED28` ends with `b 0x8216ED40` and used to swallow the whole next
    function.

Result on `default.xex`: 0 switch warnings and no function swallowing another. `AI2C.dll` is unchanged.

### 0002: missing instructions

Before, each of these was silently dropped from the generated code or turned into `__builtin_debugtrap()`.

| Instruction | Where it shows up in Naruto |
|---|---|
| `bdnzt` | `memcmp` |
| `eqv` | `MEM_p_Alloc`, `INT_PointInTriangle` |
| `mulhdu` | Bink and the engine |
| `frsqrte` | Bink |
| `dcbst` (no-op) | `TEX_XeLoadStreamedTexture` |
| `vandc` | `D3D::_pow` |
| `vpkswss` | audio FFT |
| `vctuxs` and `vsel128` | `StoreTexel` (XGRAPHICS) |
| `vupkd3d128` FLOAT16_2 and FLOAT16_4 | `D3DXFloat16To32Array`, `XMConvertHalfToFloatStream`, `LoadTexel*_16F` |
| `vpkd3d128` FLOAT16_2 | `D3D::ComputeClearColor` |
| `blrl` (without `skip_lr`) | `_CallSettingFrame` |

Half floats follow the XDK 6274 reference code (`xmconvert.inl`). Exponent 31 is a regular number, with no
inf/NaN. Float to half saturates at `0x7FFF` and rounds to nearest even. The element layout of
FLOAT16_2/FLOAT16_4 comes from how the XDK itself uses them. The helpers (`ppcHalfToFloat`, `ppcFloatToHalf`,
`simde_mm_vctuxs`) live in `ppc_context.h`.

Validation done: the game's `memcmp` and `D3DXFloat16To32Array`, recompiled, were run against the host
`memcmp` and against independently computed values. The generated translation units that use the new
instructions compile with clang-cl.

### 0003: non-trapping division

Division by zero and `INT_MIN / -1` do not trap on PowerPC (the result is undefined), but the C++ that
XenonRecomp emitted (`a / b`) traps on x86. `divw`, `divwu`, `divd` and `divdu` now give 0 for a zero divisor
and wrap around on overflow (`divw` divides in 64 bits). The game has 1,046 of these instructions; the first
one to hit a zero divisor is in `D3D::InternalScalerSetup`, on the first frame.

### 0004: module names and optional helpers

The game is two modules: `default.xex` and `AI2C.dll`, the AI scripts compiled to C, which `default.xex` loads at
startup and cannot run without (it has no AI interpreter). Both are recompiled and linked into one executable.

- **`module_name`.** Function names come from addresses (`sub_88...` cannot collide with `sub_82...`), but the
  register helpers (`__restgprlr_14`...), the entry point (`_xstart`) and the mapping table (`PPCFuncMappings`)
  had fixed names. With `module_name = "ai2c"` they become `__restgprlr_14_ai2c`, `_xstart_ai2c` and
  `PPCFuncMappings_ai2c`. Import thunks
  (`__imp__Name`) keep their names, so both modules share one kernel implementation. Without the key the output
  is unchanged.
- **Helpers set to 0.** Every helper address was required. `AI2C.dll` has no vector code and no VMX helpers, so
  an address of `0` now means the module has none; only a missing key is still an error.

### 0005: image size with basic compression

With basic compression (data blocks, each followed by zeros), `Xex2LoadImage` sized the image buffer by the
sum of the blocks, but `image.size` reports the image size of the security info. In `AI2C.dll` the blocks end
at `0x3B8000` of `0x3C0000`, so copying `image.size` bytes read past the buffer. The buffer now covers the
whole image, with the rest zero (as the console loader leaves it). `default.xex` is unaffected (its sizes
match).

### 0006: funclets keep their registers in the context

The `*_as_local` options turn registers into local variables of each recompiled function, which is only
right when no register carries a value between functions outside the calling convention. In Naruto the
compiler's SEH funclets do:

- **What a funclet is.** The body of a `__finally` block (or an `__except` filter), emitted as a separate
  function right after its parent. On the normal path the parent calls it with
  `addi r12, rN, <frame size>` and `bl`. The funclet is part of the parent's body: it rebuilds the parent's
  frame pointer from `r12` and reads and writes the parent's non-volatile registers (for example
  `mr r3, r30` before `_unlock`).
- **What would break.** With local registers the funclet gets zeros in place of the parent's values. This
  was found in the generated code before the first run: the funclet of the thread start-up wrapper
  (`0x82184364`, parent `0x82183AD0`) loads from `r27 + 1408` with its own, zero `r27`.
- **The fix.** `Recompiler::FindContextRegisterFunctions` finds every `bl` preceded by `addi r12, rN, imm`
  whose target is not a register save/restore helper. The target, the function holding the call and the
  save/restore helpers are recompiled with every register in the context, as without the options. Any
  other function follows the calling convention, so the two kinds can call each other. In both modules
  `addi` is the only instruction that sets `r12` right before a call, apart from the two callers of the
  stack probe (`rlwinm`).
- **setjmp.** The 17 functions that call `setjmp` are recompiled the same way. `longjmp` brings the
  registers back to their values at the `setjmp`, which the generated code does by restoring a copy of
  the context; a local register would keep whatever it held at the `longjmp`.
- **The context keeps every register.** The `PPC_CONFIG_*_AS_LOCAL` macros, which removed the local
  registers from `PPCContext`, are no longer written to `ppc_config.h`: the functions above still use
  them, and the modules of one runtime must agree on the layout. The cost is that host code naming one
  of those registers (`ctx.r31`, `ctx.cr6`) now compiles and reads a value no guest function keeps:
  hooks take what they need from the argument registers, `r1`, `r13` and `lr`.

Found: 42 funclets with 39 parents in `default.xex` (35 of the funclets in the CRT, `0x822C5000` to
`0x822D8200`) and 4 funclets with 8 parents in `AI2C.dll`.

How it was checked: `tools/dev/local_registers.py`, a liveness pass over the generated C++ (per function,
the local registers read on some path before being written, with calls to functions that never return
ending a path). Run it again after a change to the options, to the recompiler or to the function
analysis. After the patch it reports three functions that are called directly: the stack probe `0x8253B2D4`, which takes
its size in `r12` and now probes nothing (guest stacks are committed on touch); `0x824D0830` (D3DX),
a read on a path its own conditions exclude; and `0x8839913C`, reached only from a duplicate of
`0x88399020` that starts 8 bytes early, on the exception data, and is never called.

### Not covered by the patches

These are configuration items, in `config/<module>.toml` and `config/<module>_switch_tables.toml`:

- `invalid_instructions`:
  - zero padding;
  - exception handler data blocks of `default.xex` (`0x822C6B90` and `0x826BE634`, 8 bytes);
  - the XEX export table at the end of `.text` (`0x00485645`: `0x2D8` bytes in `default.xex`, `0x34` in
    `AI2C.dll`).
- Jump tables XenonAnalyse does not recognize in `default.xex`: `0x8264D590`, `0x823EFDCC` and `0x824F85A4`. In
  `AI2C.dll` its 53 tables are complete.

## plume

### 0001: D3D12 swap chain and adapter cleanup

Bugs in `plume_d3d12.cpp`, all still present upstream at `b619e50`:

- **Adapter selection.** When the shader model query of an adapter failed, the `continue` sat inside the
  query loop, so the loop went on with the device and adapter it had just released. The adapter is now
  skipped after the loop.
- **Failed swap chain creation.** The `IDXGISwapChain1` leaked when `MakeWindowAssociation` failed, and the
  destructor then walked `desc.textureCount` textures that were never created. It now walks `textures`.
- **Failed resize.** A failed `ResizeBuffers` left the textures released, and the next resize released them
  again through null pointers. Null textures are now skipped.
- **Frame latency handle.** The waitable object from `GetFrameLatencyWaitableObject` was never closed.

### 0002: Vulkan validation on request

Upstream turns on the validation layer and object names only in builds without `NDEBUG`, and creates no
debug messenger: the runtime is always built in Release, so a Vulkan run had no validation at all, and the
layer's messages could only go to the layer's own output.

- `CreateVulkanInterface` takes a `VulkanValidationDesc`: when `enabled`, the interface turns on
  `VK_LAYER_KHRONOS_validation` if it is installed, with `VK_EXT_debug_utils` from the loader or the
  layer. Its warnings and errors go to the given callback, through a messenger that is also chained to
  the instance creation, so the creation and destruction of the instance report too.
- `synchronization` adds synchronization validation through `VK_EXT_validation_features` when the layer
  offers it.
- `VulkanInterface` reports what it turned on (`validationEnabled`, `debugUtilsEnabled`,
  `synchronizationValidationEnabled`). Object names (`setName`) work whenever debug utils is enabled and
  are skipped otherwise. Before, they called `vkSetDebugUtilsObjectNameEXT` whenever the build had
  names compiled in.
- Without a description, or with `enabled` false, nothing is turned on, in any build type.

### 0003: Vulkan swap chain ordering

What the renderer's presents on Vulkan ran into, or would have, in `VulkanSwapChain`:

- **The first barrier after an acquire.** Command lists wait for the acquire semaphore at the color
  attachment output stage (`executeCommandLists`), but a barrier starts from the stages of the last
  barrier on the texture, and the present barrier leaves `NONE`: the layout transition of the next
  frame started at `TOP_OF_PIPE`, before the wait, and could write the image while the presentation
  engine still read it. `acquireTexture` now marks the acquired image as last used by the graphics
  stages, so the transition waits for the semaphore.
- **Out of date.** An acquire or present that returns `VK_ERROR_OUT_OF_DATE_KHR` now makes
  `needsResize()` true until the next `resize()`; before, only a change of the window size did, and
  a swap chain the surface no longer accepted stayed in use.
- **The old swap chain.** `resize()` destroyed the old swap chain as soon as its replacement existed,
  while presents of it could still be queued. It now waits for the queue to go idle first. Present
  ids also start over with the new swap chain, so that a present wait does not ask it for an id it
  never presented.
- **Refresh rate.** `getRefreshRate()` called `vkGetRefreshCycleDurationGOOGLE` whether or not the
  device had `VK_GOOGLE_display_timing`; it now returns 0 without it.

### 0004: Vulkan two-component integer formats

`toVk(RenderFormat)` in `plume_vulkan.cpp` has no case for `R32G32_UINT` and `R32G32_SINT` (the D3D12
backend maps both; still missing upstream at `b619e50`), so they fell to the `default` and became
`VK_FORMAT_UNDEFINED`. The draw pipelines describe every two-dword vertex element as `R32G32_UINT`, so their
vertex input was invalid. With the validation layer on, checking those pipelines made the layer's memory
grow without bound (about 1.2 GB/s, until the system ran out of commit).

### 0005: Vulkan dynamic blend constants and stencil reference

The renderer sets the blend constant color and the stencil reference per draw (D3D12's
`OMSetBlendFactor` and `OMSetStencilRef`). Upstream's Vulkan pipelines bake the stencil reference of the
description and leave the blend constants at zero, with no way to set either on the command list, so
blending with the constant color and stencil tests against a draw's reference were wrong. Folding the
values into the pipelines would multiply them.

- `RenderGraphicsPipelineDesc` gains `dynamicBlendConstantsEnabled` and `dynamicStencilReferenceEnabled`;
  the Vulkan pipeline then declares `VK_DYNAMIC_STATE_BLEND_CONSTANTS` / `VK_DYNAMIC_STATE_STENCIL_REFERENCE`.
  The D3D12 backend ignores both: the command list holds those values there anyway.
- `VulkanCommandList::setBlendConstants` and `setStencilReference` (both faces) set them. They are members
  of the Vulkan list only, not of `RenderCommandList`, so the other backends are untouched.

Not a bug, checked along the way: the depth-stencil attachment views of a framebuffer have only the depth
aspect, which the spec allows (an attachment view of a depth/stencil image uses both aspects whatever its
aspect mask says).

### 0006: Vulkan coherent upload and readback memory

`VulkanBuffer::map()` and `unmap()` neither flush nor invalidate, and VMA, asked only for host access,
may place an upload or readback buffer in host-visible memory that is not coherent. The renderer keeps
its upload chunks mapped for their whole life and writes them every frame, and reads the readback of
the save thumbnail right after the copy, so on such memory the GPU could read stale constants and
vertices and the host a stale thumbnail. `UPLOAD` and `READBACK` buffers now require
`VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`; Vulkan guarantees a host-visible memory type that has it.
Desktop drivers give coherent memory already, so nothing changes there.

### 0007: Vulkan render pass reuse

Upstream ends the active render pass on every `setFramebuffer`, even for the framebuffer it is already
drawing into, and on every `barriers` call, even one that changes no layout. The renderer binds the
draw's targets before each draw (a barrier to the target's write layout, then `setFramebuffer`), as
D3D12 needs, so on Vulkan every draw ended the pass, put a full pipeline barrier, and began the pass
again with LOAD and STORE: in the village scene the GPU was busy 76-84% of the time, against 8% on
D3D12.

- `setFramebuffer` with the framebuffer of the active render pass keeps the pass.
- A `barriers` call made only of texture barriers that keep attachments of the active pass in the
  layout the pass uses (color in `COLOR_WRITE`, depth in `DEPTH_WRITE`, or `DEPTH_READ` for a read-only
  depth attachment) does not end the pass: the pass orders its own attachment writes. The barriers are
  held (one per texture) and recorded right after the pass ends, whatever ends it, so the writes of the
  pass are still ordered before what follows (another pass on the same texture, a copy, a read).
- Any other barrier ends the pass as before.

Result in the village scene of Player Save 2 (paced at 30 frames a second): GPU busy 8.3% of the time
(D3D12 7.8%), 2.75 ms of GPU work a frame instead of 25-28 ms; synchronization validation stays clean.

### 0008: Vulkan pipeline cache

Upstream creates every pipeline with `VK_NULL_HANDLE` as its cache, so on Vulkan the application has
no way to keep compiled pipelines between runs (a driver may keep its own on-disk cache, but nothing
requires it to). `VulkanDevice` gains a `VkPipelineCache` that graphics and compute pipelines are
created through, `resetPipelineCache(data, size)` to start it from a previous run's data (an empty cache
when the driver refuses the data) and `getPipelineCacheData()` to read it back (retried while other
threads grow it). Both are members of the Vulkan device only. The renderer keeps the data in a file of
its own, behind a header it checks before handing the data to the driver (`gpu/renderer/backend.cpp`).

## XenosRecomp

Findings from the shaders of Naruto: Rise of a Ninja (`game/xeshader.sdb`: 4,467 containers, 3,322 unique,
1,513 vertex and 1,809 pixel shaders). The reference for what each shader does is the game itself: the SDB
carries the original HLSL of its 20 shader families, and every shader was disassembled with JadeRE
(`EngineSrc/Tools/JadeRE`, `shader-extract`, identical to the XDK's `xsd.exe`). Upstream translates every
shader without a recompiler error; the patches cover what it dropped or could not compile.

### 0001: shader databases

- The SDB packs its containers back to back, so they sit at odd offsets (all 4,467 are at an offset of 1
  modulo 4). The scan stepped 4 bytes at a time and found none; it now steps 1 byte.
- A single input file that does not start with a container (a shader database) is scanned like a directory,
  so the build passes `game/xeshader.sdb` directly instead of a folder holding it.

### 0002: report compile failures

The asserts that checked the DXC output are compiled out in Release, so the first shader that did not compile
crashed the run on a null blob. Every shader is now tried, the HLSL of the failed ones is written to
`<output>.failed/<hash>.hlsl`, and the run exits with an error.

### 0003: vertex texture fetch

Skinning reads the bone matrices from a 1D texture in the vertex shader (`TFetchBoneMatrix` in
`01_vscommon.hlsl`; 1,062 `tfetch1D` in 118 vertex shaders of the SDB, 97 of them unique): three fetches per
bone with `OffsetX` 0.5, 1.5 and 2.5 texels. Before, these shaders did not compile
(`s16_Texture1DDescriptorIndex` undeclared).

- Vertex shader sampler `sN` is texture fetch constant `16 + N` (`g_BoneMatrixSampler`, register `s0`, is
  fetched from `tf16`). Descriptor slots in the shared constants are now indexed by fetch constant, 32 per
  texture dimension and for the samplers, so vertex and pixel shader samplers no longer collide. The rest of
  the shared constants moved from `c16` to `c32` (from byte 256 to 512 on SPIR-V).
- `tfetch1D` samples the texture as a 2D texture one texel high, at the base level, with `OffsetX` applied in
  texels.

### 0004: texture gradients

`getGradients` was dropped silently (115 in 89 cel pixel shaders, from `fwidth()` in `33_pscell.hlsl`). It now
gives `(dS/dX, dS/dY, dT/dX, dT/dY)`. The order comes from the game: `fwidth(x)` compiles to
`getGradients r2._xy_, r2.xx` followed by `|r2.y| + |r2.z|`.

### 0005: raw vertex fetch, spec constants as a shared constant

**Vertex fetch.** Upstream declares each vertex input by its usage with a fixed type and expects the host to
convert the vertex data into an input layout of typed formats (with a TEXCOORD swap and an R11G11B10 normal path
made for another game). The D3D library of the game patches the fetch instructions of every vertex shader at run
time for the bound vertex declaration and strides (`D3D::PatchVertexShaderToMatchVertexDeclaration`): format,
offset, stride, fetch constant and destination swizzle can all differ from the template in the shader database.

- Every vertex input is now `uint4`, the raw dwords of its element in guest byte order, bound by the host as
  `R32[G32[B32[A32]]]_UINT`.
- A vertex fetch writes `vfetch(input, g_VertexFetchDescriptor(i))`, where `i` is the element's index in the
  container. `vfetch` (`shader_common.h`) applies the endian swap of the fetch constant and decodes every vertex
  format the GPU has (8_8_8_8, 2_10_10_10, 10_11_11, 11_11_10, 16-bit and 32-bit, fixed point and float, signed
  ranges, exponent adjustment), then the destination swizzle. The descriptor is built per draw by the host from the
  fetch instruction as the library patched it. Only the components the template writes are assigned.
- The descriptors live in the shared constants at `c34`-`c37` (16 elements).

**Spec constants.** Shaders that read `g_SpecConstants()` (the alpha test branch in every pixel shader) were
compiled as DXIL libraries, to be linked at run time with a library that defines the function. It is now a value
of the shared constants (`c32.y`, replacing `g_SwappedTexcoords`): every shader compiles once as a plain shader,
and the branch on it is uniform. The library target of `DxcCompiler::compile` is gone.

Result: 3,322 of 3,322 compile, with one DXIL per shader.

### 0006: user clip planes

The game's planar reflection pass (a 640x360 4x target in the village) enables user clip plane 0
(`PA_CL_CLIP_CNTL` `0x00080001`), which upstream drops: on the GPU it is fixed function, after the vertex
shader. Every vertex shader now writes `SV_ClipDistance0` (planes 0-3) and `SV_ClipDistance1` (planes 4-5), the
dot product of the exported position with each plane, before the host's half-pixel offset, so in the clip space
the GPU clips in. The planes live in the shared constants at `c38`-`c43` (byte 608 on SPIR-V); a disabled plane
is zero and clips nothing. The clip distances come after the interpolators, so the pixel shader inputs keep
their registers.

### 0007: vertex input locations

Vulkan matches vertex attributes by location only. Upstream took each input's location from
`USAGE_LOCATIONS`, a table written for Unleashed Recompiled (its own comment says so), where Position1 and
TexCoord7 share location 15 and usages missing from the table get no location. The host binds element `i` of
the declaration at location `i`, so on Vulkan most draws would have read the wrong streams (in a sampled
shader the elements Position0, Color0, Color1, Normal0, Tangent0, TexCoord0, TexCoord1 sat at 0, 8, 11, 1,
2, 4, 5). Element `i` is now at location `i`, the index that also selects its fetch descriptor
(`g_VertexFetchDescriptor(i)`), and the table is gone. DXIL matches by semantic and does not change.

## ffmpeg

FFmpeg n7.1.5, shallow. The runtime builds only libavutil and libavcodec with the `xma1` decoder, from the
configuration `scripts/ffmpeg-configure.ps1` writes to `cmake/ffmpeg` (see that script).

### 0001: XMA, a single stream without the 4096-sample hold-back

`xma_decode_packet` (`libavcodec/wmaprodec.c`) keeps the last 4096 samples in its FIFOs until the end of the
input, so they can be trimmed at the end of a file and the streams of a multi-stream file stay aligned. The
runtime feeds one stream of an XMA hardware context packet by packet and never ends it: with the hold-back,
short sounds (one 2048-byte packet is often less than 4096 samples) would come out only with the next
sound. With a single stream the samples now come out as soon as their packet is decoded; several streams
keep the hold-back.
