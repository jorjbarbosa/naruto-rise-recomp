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
| `DLL XEX not found` for `ai2c2.dll` | Supply the extracted DLC module or remove only its manifest block for a base-game build |
| Missing generated header or DLL target | Run code generation before configuring the application and reconfigure after changing modules |
| Failure to copy `extract-xiso.exe` | Run `setup.ps1`; the complete build includes the installer helper |
| Out of memory during compilation | Reduce concurrency: `cmake --build app/out/build/win-amd64-release --parallel 2` |
| Packaging reports a missing cache | Check `packaging/shader_cache/` and the cache copied to the output; the script requires files for title `555307E5` |
| Debug CRT dependency in a release | Rebuild the SDK and application in Release; do not reuse Debug DLLs |

## Runtime issues

### The launcher cannot find the game

Select the folder directly containing `default.xex`. Use `game/` in a distribution; the checkout manifest uses `game_root/`. A folder that merely contains another folder with the game is not the correct root. You can also set `custom_game_root` in the TOML file or pass `--game_data_root` to the executable.

### Missing DLL at startup

Keep runtime and module binaries alongside the game, together with launcher assets. Install the Visual C++ x64 Redistributable. For character DLCs, specifically check `narutorise_ai2c2.dll`; importing content through the launcher does not compile this module.

### Stretched image on a 16:9 monitor

The source TOML uses `ultrawide_target_aspect = 2.3889`. Choose 16:9 in the launcher or set `0.0`. For ultrawide, confirm that the SDK was built with `ultrawide-presenter.patch` applied.

### `projection constant mismatch` in the log

The ultrawide patch found an unexpected value at the projection address. Check the original executable revision and whether another patch changed the constant. This check prevents the aspect-ratio factor from being applied to unexpected data.

### Black flickering or graphics issues

Check `execute_unclipped_draw_vs_on_cpu = true`, `d3d12_readback_resolve = true`, and the SDK patches used for the build. For low performance, reduce the scale in the launcher and compare using the `F1` overlay. The bundled cache does not guarantee that shader compilation will never occur during gameplay.

### Preferences reset after a build

CMake copies `app/narutorise.toml` into the output directory. Adjust the development template or restore your local configuration after building.

### DLC is missing or fails to load

Run the game after importing content to finish installation. Check content and `.header` files in Documents, installation log messages, and the recompiled DLC DLL. If you customized `user_data_root`, remember that the launcher still uses Documents. See [DLCs](dlc.md).

## Collecting diagnostics

Default logs are written to `logs/` next to `narutorise.exe`. The FPS overlay (`F1`) shows the **guest** frame rate (the game's own frame production, which is what game speed follows) alongside the **host** rate; slow motion with a high host rate means the guest rate is below target. For additional details, temporarily set:

```toml
log_level = "debug"
```

When reporting a problem, include:

- build/commit, Windows version, CPU, GPU, and driver version;
- guest and host FPS from the `F1` overlay (the guest rate drives game speed; the host rate is context);
- reproduction steps and expected result;
- game revision and DLC or Title Update usage;
- resolution, aspect ratio, and internal scale;
- relevant log excerpts or the complete log, reviewing personal paths before sharing.

Restore `log_level = "info"` after diagnosis to reduce log volume.
