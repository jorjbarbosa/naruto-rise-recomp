# Troubleshooting

[Documentation](README.md) · [Build guide](build-and-run.md)

## Build issues

| Symptom | What to check |
| --- | --- |
| `clang`, `ninja`, or Windows libraries not found | Use the VS 2022 x64 development environment with LLVM and the Windows SDK available |
| `UTF-16 (LE) byte order mark detected` in `ffx_api_dll.rc` when building the SDK | The build ran in a shell without the Windows SDK on `PATH`, so CMake fell back to `llvm-rc`-based preprocessing, which cannot read FidelityFX's UTF-16 resource script. Run the configure/build from the VS x64 environment (`vcvars64.bat`) so `rc.exe` is found, then rebuild |
| Preset parsing error | Use CMake 3.26+; presets use schema version 6 |
| `ReXGlue SDK not found` | Build and install the SDK, then pass its prefix through `CMAKE_PREFIX_PATH` |
| A patch does not apply | Compare the SDK commit and local changes with the patch; do not discard modifications to force application |
| `DLL XEX not found` for `ai2c1.dll`, `ai2c2.dll`, or `ai2c3.dll` | Supply the matching extracted DLC module or remove the DLC manifest blocks for a base-game build |
| Missing generated header or DLL target | Run code generation before configuring the application and reconfigure after changing modules |
| Out of memory during compilation | Reduce concurrency: `cmake --build app/out/build/win-amd64-release --parallel 2` |
| Packaging reports a missing cache | Copy `555307E5.xsh` and `555307E5.rtv.d3d12.xpso` from `<effective cache_root>/shaders/shareable/` to `packaging/private/shader_cache/` (or configure the application with `-DNARUTORISE_SHADER_STORAGE_DIR=<folder>`), then package again |
| Debug CRT dependency in a release | Rebuild the SDK and application in Release; do not reuse Debug DLLs |
| `Native bootstrap requires NATIVE_RENDERER_PLUME_SOURCE` | Pass a Plume checkout with `-DNATIVE_RENDERER_PLUME_SOURCE=<folder>`; see [Native renderer](build-and-run.md#native-renderer-optional) |
| `plume source must be clean at ...` or `allocator source must be clean at ...` | Check out the pinned commits in Plume and its `contrib/D3D12MemoryAllocator` submodule, without local changes |
| `The shader cache of the native renderer needs .../xeshader.sdb` | Point `NARUTORISE_GAME_DIR` at the folder with your game files (for example `game_root/`) |
| `The shader cache of the native renderer needs .../dxc.exe` | Fetch the XenosRecomp submodules: `git submodule update --init --recursive` |

## Runtime issues

### The setup screen does not accept the game

Select the original ISO, or `default.xex` in the folder that directly contains it. A folder that merely contains another folder with the game is not the correct root. In a distribution, `game/` beside the executable or AppImage is also found; the checkout manifest uses `game_root/`. You can also set `custom_game_root` in the TOML file or pass `--game_data_root` to the executable. Hold **Shift** while opening, or pass `--setup`, to return to the setup screen.

### Missing DLL at startup

Keep the runtime and module binaries alongside the game, as the ZIP extracts them. Install the Visual C++ x64 Redistributable. For character DLCs, check the matching `narutorise_ai2c1.dll` (Shikamaru), `narutorise_ai2c2.dll` (Jiraiya & Sarutobi), or `narutorise_ai2c3.dll` (Choji & Temari). Importing content through the DLC menu does not compile these modules.

### Stretched image on a 16:9 monitor

An ultrawide aspect ratio is selected with Xenos. Set `ultrawide_target_aspect = 0.0`, the default. For ultrawide, confirm that the SDK was built with `ultrawide-presenter.patch` applied.

### `projection constant mismatch` in the log

The ultrawide patch found an unexpected value at the projection address. Check the original executable revision and whether another patch changed the constant. This check prevents the aspect-ratio factor from being applied to unexpected data.

### Black flickering or graphics issues

Check `execute_unclipped_draw_vs_on_cpu = true`, `d3d12_readback_resolve = true`, and the SDK patches used for the build. For low performance, reduce **Resolution** in **Settings → Display** and compare with the **FPS Overlay** (**Settings → Advanced → On-screen indicators**). The bundled cache does not guarantee that shader compilation will never occur during gameplay.

### Native renderer

| Symptom | What to check |
| --- | --- |
| **Settings → Display** has no **Renderer** option | The build does not include the native renderer; see [Build and run](build-and-run.md#native-renderer-optional) |
| Bars on an ultrawide monitor | Expected: the native renderer stays at 16:9; for 21:9 or 32:9, set `renderer = "xenos"` and `ultrawide_target_aspect` in `narutorise.toml` (Xenos has no setup screen or in-game menu) |
| `F1` does not open the menu | The menu belongs to the native renderer; with Xenos, `F1` toggles the SDK's FPS overlay. If you remapped the shortcut in Controls, use the new key or `Back+Start` |
| The frame rate stays at 30 in the open world or 60 in fights | Check the **Frame rate** slider; with VSync on, the rate does not exceed the monitor's refresh rate. Rates just above the game's step follow the step, see [Frame rate](renderers.md#frames-between-the-logic-steps) |
| The rate varies at 120 FPS | Busy areas may not reach 120; choose 60 |
| An object or effect is missing for a moment | Its pipeline is compiling the first time it appears |
| Closing takes a few seconds | A known issue, mostly with a controller connected or right after a fight |

See the [known issues](renderers.md#known-issues). When a problem appears only with the native renderer, check whether it also happens with `frame_rate = 30` and with Xenos, and mention the result in the report.

### Linux AppImage

| Symptom | What to check |
| --- | --- |
| The AppImage does not start | It needs glibc 2.43 or newer (`ldd --version`); on an older system, use the Windows ZIP through Proton |
| `fuse` or `libfuse` errors | Install FUSE, or run `./NarutoRise-<version>-x86_64.AppImage --appimage-extract-and-run` |
| A folder opens behind the game | Known on KDE Wayland with the game in fullscreen: switch to windowed mode (`Alt+Enter` or `F11`) |
| No sound or music | Run the AppImage with `--test-audio-wait`; it must finish with `PASS` |
| The save icon keeps spinning | Seen once on Linux and under investigation; keep a copy of your saves and attach the exported session to the report |

Data is in `~/.local/share/narutorise` (`$XDG_DATA_HOME`), the rebuildable cache in `~/.cache/narutorise`.

### Preferences reset after a build

CMake copies the factory template into `defaults/narutorise.toml`; the active configuration remains in the data folder. If preferences appear missing, confirm the active/pending location and session overrides in Settings → Advanced. See [Data folder and recovery](data-folder.md).

### DLC is missing or fails to load

Run the game after importing content to finish installation. Check content and `.header` files in the effective guest root, installation log messages, and the recompiled DLC DLL. The game menu shares the runtime root, including `user_data_root` overrides. See [DLCs](dlc.md).

## Collecting diagnostics

Default logs are written to `logs/` in the active data folder, shown in Settings → Advanced. Each session also keeps a folder under `incidents/` with its log and, after a crash, a dump: **Settings → Advanced → Diagnostics → Export previous session** packs it into a ZIP to attach to the report, and **Capture thread snapshot** records a hang. See [Local diagnostics](crash-reports.md).

With the native renderer, the **FPS Overlay** (**Settings → Advanced → On-screen indicators**) shows the presented rate and frame time. With Xenos, the `F1` overlay shows the **guest** frame rate (the game's own frame production, which is what game speed follows) alongside the **host** rate; slow motion with a high host rate means the guest rate is below target. For additional details, temporarily set:

```toml
log_level = "debug"
```

When reporting a problem, include:

- build/commit, operating system (Windows version or Linux distribution), CPU, GPU, and driver version;
- the renderer (Xenos or native) and, with the native renderer, the API (Direct3D 12 or Vulkan) and the frame rate;
- the rate from the native FPS Overlay, or guest and host FPS from the Xenos `F1` overlay (the guest rate drives game speed; the host rate is context);
- the exported session ZIP after a crash or hang;
- reproduction steps and expected result;
- game revision and DLC or Title Update usage;
- resolution, aspect ratio, and internal scale;
- relevant log excerpts or the complete log, reviewing personal paths before sharing.

Restore `log_level = "info"` after diagnosis to reduce log volume.
