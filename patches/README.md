# Submodule patches

Our changes to the submodules in `tools/`, kept as patches instead of a fork. Each folder is named after a
submodule and holds `.patch` files (output of `git diff` inside the submodule), applied in alphabetical order.

## Usage

```powershell
git submodule update --init --recursive
./patches/apply.ps1            # apply; safe to rerun, already applied patches are skipped
./patches/apply.ps1 -Revert    # undo, in reverse order
```

After applying, `git status` in the project shows `tools/XenonRecomp (modified content)`. That is expected:
the submodule stays on the upstream commit and the changes live only here.

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

### Not covered by the patches

These are configuration items, in `config/default.toml` and `config/default_switch_tables.toml`:

- `invalid_instructions`:
  - zero padding;
  - exception handler data blocks (`0x822C6B90` and `0x826BE634`, 8 bytes);
  - export table at the end of `.text` (`0x00485645`, `0x2D8` bytes).
- Jump tables XenonAnalyse does not recognize: `0x8264D590`, `0x823EFDCC` and `0x824F85A4`.
- Two modules (`AI2C.dll`): indirect call lookup over two code ranges and per-module symbol names. The game
  has a path without the DLL (it interprets the AI bytecode), so this can wait.
