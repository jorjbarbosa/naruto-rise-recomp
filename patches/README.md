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
