# Project architecture and analysis

[Documentation](README.md)

This analysis is based on the code and scripts present in the checkout. Features identified in the code do not constitute complete validation of every game stage, revision, or platform.

## Overview

```text
Xbox 360 files + manifest + seeds
                  │
           ReXGlue codegen
                  │
        C++ in app/generated/
                  │
        Clang + CMake + Ninja
                  │
    Native EXE + DLLs + runtime
                  │
       narutorise.exe → setup or game
```

PowerPC code is translated ahead of time into C++. The ReXGlue runtime provides the services required by the original game, including guest memory, kernel, content, audio, input, and Xenos GPU support. The game draws through the native renderer in `app/src/gpu/` (the default), with Direct3D 12 or Vulkan through Plume, or the SDK's Xenos GPU emulation (D3D12 on Windows). Embedded RmlUi menus share the native renderer; no separate launcher is built.

## Structure

| Path | Responsibility |
| --- | --- |
| `app/src/` | Runtime integration, ultrawide, intros, DLCs, overlay, graphics card selection, save thumbnail guard, and input recording for tests |
| `app/src/gpu/` | Native renderer (built with `NARUTORISE_NATIVE_RENDERER_BOOTSTRAP=ON`); see [Native renderer](#native-renderer) |
| `app/src/menu/`, `app/ui/` | Setup and in-game UI, configuration/content models, localization, update notices and embedded assets |
| `app/narutorise_manifest.toml` | Entrypoint and modules to recompile |
| `app/*-seeds.toml` | Adjustments and information for analysis/code generation |
| `app/narutorise.toml` | Default runtime configuration |
| `app/generated/rexglue.cmake` | Generated CMake/SDK integration |
| `app/generated/default/`, `ai2c/`, `ai2c1/`, `ai2c2/`, `ai2c3/` | Local code generation outputs |
| `sdk/` | ReXGlue and dependencies |
| `patches/sdk/` | Reapplicable local SDK changes |
| `tools/` | ISO extractor and content utility |
| `tools/native-renderer/` | Native renderer build support: Plume integration and shader cache generation |
| `tools/xenosrecomp/` | XenosRecomp fork that translates the game's shaders for the native renderer |
| `packaging/` | Release preparation, Inno Setup, and tests |
| `game_root/` | Local game data used during development |
| `dist/` | Locally generated packages |

## Application integration

`app/src/main.cpp` registers `NarutoriseApp` using the generated entrypoint configuration. Hooks in `narutorise_app.h`:

- locate game data and Title Updates;
- apply ultrawide after loading the XEX image;
- prepare the shader cache and install DLCs;
- register `F1` and `Alt+F4`;
- update projection when the aspect-ratio cvar changes;
- remove callbacks and UI during shutdown.

`ultrawide.cpp` validates and modifies the projection constant at `0x826F5108`. Presentation depends on the SDK patch. `intro_skip.cpp` contains hooks specific to intro videos.

`graphics_device.cpp` resolves the graphics card selected by name for both renderers; `main.cpp` also exports the hybrid laptop hints (`NvOptimusEnablement`, `AmdPowerXpressRequestHighPerformance`). `save_thumbnail.cpp` guards the autosave thumbnail capture against a race between the game's render task and its save thread. `input_replay.cpp` records and replays controller input for repeatable test runs (`REX_NATIVE_INPUT_RECORD`, `REX_NATIVE_INPUT_REPLAY`).

## Native renderer

The native renderer replaces the Xenos plugin at launch unless `renderer = "xenos"`; it is the default. It hooks the game's Direct3D layer instead of executing its GPU command buffers. User-facing behavior and known issues are in [Renderers and frame rate](renderers.md).

| Path | Responsibility |
| --- | --- |
| `gpu/host/` | Configuration, guest kernel video and threading exports, and the frame lifecycle |
| `gpu/d3d/` | Hooks of the game's Direct3D functions: device, resources, render states, draws, queries, presents, and shutdown |
| `gpu/backend/` | Direct3D 12 engine on Plume and the dispatcher that translates draws on its own thread |
| `gpu/shaders/` | Embedded shader cache and pipelines, including the list compiled at startup |
| `gpu/textures/`, `gpu/buffers/` | Texture formats, tiling and uploads; vertex and index buffers |
| `gpu/common/` | Shared helpers: guest memory access, hashes, records, and the resolution scale |
| `gpu/interpolation/` | Logic tick and the transforms interpolated between ticks (`frame_rate` above 30) |
| `gpu/diagnostics/` | Records and measurements, enabled with `REX_NATIVE_DIAGNOSTICS=1` |
| `gpu/settings.cpp` | Graphics options, under the SDK names where the SDK has one |

The shader cache is generated at build time from the local `xeshader.sdb` by `tools/native-renderer/shader-cache.cmake`, using the XenosRecomp fork and DXC, and embedded in the executable. It derives from the game files, so it is never committed.

## SDK patches

| Patch | Purpose |
| --- | --- |
| `codegen-alt-version-dll-modules.patch` | Code generation for alternative module versions, required by the DLC workflow |
| `d3d12-queue-fence-result-check.patch` | Checks the results of the fence signal and wait separately in the D3D12 command processor's submission wait |
| `guest-frame-stats.patch` | Enables the guest frame rate measurement in Release builds, feeding the F1/F3 overlays |
| `mnk-input-driver.patch` | Keyboard/mouse driver: standalone modifiers, immutable per-port host profiles, mapped mouse directions, and direct camera deltas scoped to J1; retains the previous camera integration |
| `z-controls-v4.patch` | Device identity shared with the native controls UI, metadata refresh, actual SDL capabilities, and extra keyboard/mouse names. Applied after the MnK patch; both recognize an already applied SDK |
| `obdosdevices-relative-paths.patch` | Relative device path handling |
| `physical-memory-trace.patch` | Physical memory diagnostics |
| `ultrawide-presenter.patch` | Anamorphic presentation in the graphics backend |
| `unclipped-draw-vs-on-cpu-default.patch` | Draw extent calculation adjustment to avoid EDRAM corruption |

`apply_sdk_patches.ps1` applies files in filename order and detects already applied patches using `git apply --reverse --check`. Rebuild and reinstall the SDK after changing these patches.

## Launcher and distribution

The launcher saves the TOML file and starts the game with `--game_data_root`. For DLCs, it stages packages in `dlc/` or copies extracted content into Documents. ISO extraction belongs to the installer, through a native helper and `extract-xiso`, rather than the shipped launcher.

`package-release.ps1` configures Release mode, refreshes targets, checks inputs, inspects PE imports to reject debug CRTs, and assembles the distribution in a private staging directory. Previous releases are preserved in `dist/previous-release-*/`.

## Strengths

- Clear separation between port code, generated sources, launcher, and distribution.
- Explicit manifest for the base game and DLC engine, with separate seeds.
- SDK modifications stored as reapplicable patch files.
- User data preservation in installer and packaging workflows.
- Synthetic ISO import and installation tests whose fixtures do not require original game data.

## Areas to address and next steps

1. **SDK reproducibility:** the SDK is a submodule pinned at a commit, with the local patches applied on top. Verifying a clean clone with all patches before each release keeps builds reproducible.
2. **Setup and manifest:** setup's final instructions suggest `init --force`, while the manifest contains DLC customizations. The documented workflow uses `codegen` directly to preserve them.
3. **Optional DLC builds:** the current manifest requires all three DLC engine input files. Packaging includes each declared revision and rejects missing generated module binaries. A dedicated base-game preset or manifest would be a useful improvement.
4. **Portability:** Linux/macOS/ARM64 presets exist, but end-to-end validation and a complete non-Windows launcher graphics path are still needed.
5. **Revision compatibility:** seeds and hooks use specific addresses. A table of hashes/revisions and test results for the game, DLCs, and TUs is missing.
6. **Custom user data:** the runtime accepts `user_data_root`; the DLC launcher directly resolves Documents. Shared path resolution would prevent mismatches in customized installations.
7. **Build configuration:** `narutorise_runtime_config` synchronizes the source TOML on each build and can overwrite preferences changed in the output folder.
8. **Native renderer:** it is the default and the newest part of the port; its open defects are listed in [Renderers and frame rate](renderers.md#known-issues). Plume is an external checkout pinned by commit rather than a submodule, and the startup pipeline list is generated from local play sessions, so a clean clone builds without it.

## Maintenance guidance

Edit seeds, the manifest, and application hooks instead of manually fixing generated C++. Keep disc data and derived sources out of version control, as specified by `.gitignore`. When modifying the SDK, update its corresponding patch and check the result in a clean checkout.

For installer changes, see the [bridge tests](../packaging/README.md). Runtime reports should include data revision, DLC/TU usage, renderer and frame rate, aspect ratio, scale, GPU/driver, and logs to help distinguish content, runtime, and presentation failures.
