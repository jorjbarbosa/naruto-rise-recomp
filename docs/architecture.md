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
       Launcher → game execution
```

PowerPC code is translated ahead of time into C++. The ReXGlue runtime provides the services required by the original game, including guest memory, kernel, content, audio, input, and Xenos GPU support. On Windows, the game uses the D3D12 backend by default; the launcher has its own D3D11 renderer.

## Structure

| Path | Responsibility |
| --- | --- |
| `app/src/` | Runtime integration, ultrawide, intros, DLCs, and overlay |
| `app/narutorise_manifest.toml` | Entrypoint and modules to recompile |
| `app/*-seeds.toml` | Adjustments and information for analysis/code generation |
| `app/narutorise.toml` | Default runtime configuration |
| `app/generated/rexglue.cmake` | Generated CMake/SDK integration |
| `app/generated/default/`, `ai2c/`, `ai2c2/` | Local code generation outputs |
| `launcher/src/` | SDL3/ImGui UI, configuration, data discovery, and content management |
| `launcher/assets/` | Launcher cover art, icon, and fonts |
| `sdk/` | ReXGlue and dependencies |
| `patches/sdk/` | Reapplicable local SDK changes |
| `tools/` | ISO extractor and content utility |
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

## SDK patches

| Patch | Purpose |
| --- | --- |
| `codegen-alt-version-dll-modules.patch` | Code generation for alternative module versions, required by the DLC workflow |
| `guest-frame-stats.patch` | Enables the guest frame rate measurement in Release builds, feeding the F1/F3 overlays |
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

1. **SDK reproducibility:** `.gitmodules` declares the SDK, but the repository root had no files recorded in the Git index when analyzed. Before publication, recording the SDK gitlink/commit and verifying a clean clone with all patches is essential for reproducible builds.
2. **Setup and manifest:** setup's final instructions suggest `init --force`, while the manifest contains DLC customizations. The documented workflow uses `codegen` directly to preserve them.
3. **Optional DLC builds:** packaging accepts a missing `narutorise_ai2c2.dll`, but the current manifest requires its input file. A dedicated base-game preset or manifest would be a useful improvement.
4. **Portability:** Linux/macOS/ARM64 presets exist, but end-to-end validation and a complete non-Windows launcher graphics path are still needed.
5. **Revision compatibility:** seeds and hooks use specific addresses. A table of hashes/revisions and test results for the game, DLCs, and TUs is missing.
6. **Custom user data:** the runtime accepts `user_data_root`; the DLC launcher directly resolves Documents. Shared path resolution would prevent mismatches in customized installations.
7. **Build configuration:** `narutorise_runtime_config` synchronizes the source TOML on each build and can overwrite preferences changed in the output folder.
8. **Licensing and publication:** a project license is missing at the root. The maintainer should choose it; dependencies already have their own terms.

## Maintenance guidance

Edit seeds, the manifest, and application hooks instead of manually fixing generated C++. Keep disc data and derived sources out of version control, as specified by `.gitignore`. When modifying the SDK, update its corresponding patch and check the result in a clean checkout.

For installer changes, see the [verification guide](installer-status.md). Runtime reports should include data revision, DLC/TU usage, aspect ratio, scale, GPU/driver, and logs to help distinguish content, runtime, and presentation failures.
