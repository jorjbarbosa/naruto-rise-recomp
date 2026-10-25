# Build and run

[Documentation](README.md) · [Configuration](configuration.md) · [DLCs](dlc.md)

This guide describes the project's **Windows x64 / Release** workflow. Run commands from the repository root in PowerShell 7.2+ with the Visual Studio x64 development environment loaded. Stop the sequence if any command fails.

## 1. Prerequisites

| Tool | Requirement |
| --- | --- |
| Git | With submodule support |
| Visual Studio 2022 or Build Tools | x64 C++ tools, MSVC libraries, and Windows SDK |
| LLVM/Clang | Version 18 or newer, with `clang` and `clang++` on `PATH` |
| CMake | **3.26+** for the project's version 6 JSON presets |
| Ninja | Available on `PATH` |
| PowerShell | 7.2+ for `setup.ps1` |
| Game files | An extracted Xbox 360 copy, including `default.xex` and `ai2c.dll` |
| Python | 3.x on `PATH`, only needed for the native renderer and the packaging tests |
| Inno Setup | 6.4+, only needed to build the optional compatibility bridge |

The `CMakeLists.txt` files declare a minimum of 3.25, but version 6 presets require CMake 3.26. The build uses C++23. Initial configuration may also download dependencies, so an internet connection is required.

Check your environment:

```powershell
git --version
clang --version
clang++ --version
cmake --version
ninja --version
$PSVersionTable.PSVersion
```

## 2. Prepare the SDK and tools

From an existing checkout:

```powershell
pwsh -File .\setup.ps1
```

The script checks tools, initializes submodules, downloads `extract-xiso.exe`, and applies the patches in `patches/sdk/`. The SDK configured in `.gitmodules` is `https://github.com/rexglue/rexglue-sdk.git`.

The translation loader and menus use the pinned JSON parser in `third_party/nlohmann_json`, through the `narutorise::json` CMake target. This application dependency does not require the SDK's private inja headers or a separate download.

If the SDK and extractor are already available, still make sure the local patches have been applied before building the SDK:

```powershell
pwsh -File .\patches\apply_sdk_patches.ps1
```

**For this existing project, follow the remaining steps in this guide instead of the `rexglue init --force` command printed at the end of setup.** The manifest already contains seeds and the DLC module mapping; `init --force` can overwrite these customizations.

## 3. Prepare the game files

Place the extracted disc contents in `game_root/`, without an extra enclosing folder:

```text
game_root/
├── default.xex
├── ai2c.dll
├── ai2c1.dll       # copy of AI2C@1.dll from Shikamaru
├── ai2c2.dll       # optional copy of AI2C@2.dll from the DLC
├── ai2c3.dll       # copy of AI2C@3.dll from Choji & Temari
└── ...            # remaining disc files and folders
```

To extract an ISO using the tool downloaded by setup, use a new destination folder:

```powershell
.\tools\extract-xiso\extract-xiso.exe -x -d .\game_root "D:\Backups\Naruto.iso"
```

The expected Title ID is `555307E5`. Identifying the title does not establish compatibility with every revision; seeds and patches contain revision-specific addresses.

### Choose a build with or without DLC

The current manifest **declares all three DLC engine modules**. Before running code generation:

- **With character DLC:** copy the three extracted engine revisions to `game_root/ai2c1.dll`, `game_root/ai2c2.dll`, and `game_root/ai2c3.dll`. See [DLCs — recompiled module](dlc.md#33-generate-the-recompiled-module).
- **Base game only:** in your local manifest, remove the three `[[modules]]` blocks with `guest_path = "ai2c@1.dll"`, `"ai2c@2.dll"`, and `"ai2c@3.dll"`, including their fields. Keep the `ai2c.dll` block and entrypoint seeds. Restore the blocks when preparing a complete DLC-enabled build.

Code generation fails when a declared module's input file does not exist. The DLL is optional in the final package, but is not automatically skipped in the manifest.

## 4. Build and install the SDK

```powershell
cmake --preset win-amd64 -S sdk -B sdk/out/build/win-amd64 -DREXGLUE_ENABLE_FIDELITYFX=ON
cmake --build sdk/out/build/win-amd64 --config Release --target install
```

The preset installs to `sdk/out/install/win-amd64/`, including `bin/rexglue.exe`. FidelityFX must be enabled when building the SDK to make its integration available to the application; enabling the option only in the application does not rebuild an already installed SDK.

## 5. Generate the recompiled C++

```powershell
.\sdk\out\install\win-amd64\bin\rexglue.exe codegen .\app\narutorise_manifest.toml
```

This reads the manifest and `*-seeds.toml` files, producing sources in `app/generated/default/`, `app/generated/ai2c/`, and the configured `app/generated/ai2c1/`, `ai2c2/`, and `ai2c3/` directories.

Run initial code generation **before configuring the application**, so CMake can find source lists and DLL targets. Subsequent builds use the `narutorise_codegen` target for incremental regeneration. After adding or removing modules, rerun code generation and CMake configuration.

## 6. Build the game

```powershell
$sdkPrefix = (Resolve-Path .\sdk\out\install\win-amd64).Path
cmake --preset win-amd64-release -S app -B app/out/build/win-amd64-release "-DCMAKE_PREFIX_PATH=$sdkPrefix" -DNARUTORISE_NATIVE_RENDERER_BOOTSTRAP=ON
cmake --build app/out/build/win-amd64-release
```

The complete build includes the game, runtime DLLs, embedded RmlUi menus/fonts, configuration and shader cache. No launcher, ISO helper or extractor is built or copied. The extractor remains a developer tool for preparing code-generation inputs.

### Native renderer (optional)

The [native renderer](renderers.md) is built into `narutorise.exe` when the application is configured with `NARUTORISE_NATIVE_RENDERER_BOOTSTRAP=ON`. The executable keeps the Xenos plugin, and the menu lets you choose Direct3D 12 or Vulkan ([Renderer](configuration.md#renderer)). It needs:

- the XenosRecomp submodules in `tools/xenosrecomp/thirdparty`, fetched by `setup.ps1` (or `git submodule update --init --recursive`);
- a clean [Plume](https://github.com/renderbag/plume) checkout at commit `d72379344dacd3dbf9f810f92ddc87e6de1845b1`, with its D3D12 Memory Allocator, Volk, Vulkan Headers and Vulkan Memory Allocator submodules initialized at the pinned revisions. Configuration stops if a required dependency is at another commit or has local changes;
- `xeshader.sdb` from your game files. The build reads it from `game/` at the repository root (`NARUTORISE_GAME_DIR`); point the variable at `game_root/` when your files are there;
- Python 3 and `spirv-val` on `PATH` (or provide the validator with `-DNARUTORISE_SPIRV_VAL=<path>`).

```powershell
git clone https://github.com/renderbag/plume.git out/plume
git -C out/plume checkout d72379344dacd3dbf9f810f92ddc87e6de1845b1
git -C out/plume submodule update --init contrib/D3D12MemoryAllocator contrib/volk contrib/Vulkan-Headers contrib/VulkanMemoryAllocator

$sdkPrefix = (Resolve-Path .\sdk\out\install\win-amd64).Path
$plume = (Resolve-Path .\out\plume).Path
$gameDir = (Resolve-Path .\game_root).Path
cmake --preset win-amd64-release -S app -B app/out/build/win-amd64-release "-DCMAKE_PREFIX_PATH=$sdkPrefix" `
  -DNARUTORISE_NATIVE_RENDERER_BOOTSTRAP=ON -DNARUTORISE_NATIVE_VULKAN=ON "-DNATIVE_RENDERER_PLUME_SOURCE=$plume" "-DNARUTORISE_GAME_DIR=$gameDir"
cmake --build app/out/build/win-amd64-release
```

The build translates the game's shaders with the XenosRecomp fork, compiles them with DXC, links the spec constant variants, and embeds the result in `narutorise.exe`, so the native renderer needs no shader compiler at run time. The shader cache derives from your game files and stays out of Git. The build keeps the translated and compiled shaders in `out/shader-store` (`NARUTORISE_SHADER_STORE`; empty disables it) and reuses them while the game files, the XenosRecomp fork, and DXC do not change; deleting the folder only makes the next build slower.

Native builds include D3D12 and Vulkan by default. The explicit `-DNARUTORISE_NATIVE_VULKAN=ON` above also enables Vulkan in an existing build folder that cached it as disabled. The build checks the dependency revisions, enables the required device extensions in a separate build copy of Plume, and embeds validated SPIR-V for the game and presentation passes. Use `-DNARUTORISE_NATIVE_D3D12=OFF` for a Vulkan-only native renderer, or `-DNARUTORISE_NATIVE_VULKAN=OFF` for D3D12 only. At least one must be enabled when the native renderer is built.

For a local validation toolchain without installing the Vulkan SDK, the optional
[`build-validation-layer.ps1`](../tools/native-renderer/vulkan/build-validation-layer.ps1)
helper builds the pinned Khronos validation layer and its dependencies under the
directory passed with `-Root`. It does not install the layer system-wide. Point
`NARUTORISE_SPIRV_VAL` at the resulting `spirv-val.exe` when configuring the game.

The menu lists only the native APIs included in that build. `native_backend = "vulkan"` selects Vulkan; an omitted key selects D3D12 when available, otherwise Vulkan. Explicitly requesting an absent API fails without switching APIs. Vulkan needs the system loader and a compatible driver, including extended dynamic state and swapchain maintenance1 (KHR or EXT) for presentation retirement. Users do not need DXC, Vulkan SDK, `spirv-val` or validation layers.

The list of pipelines compiled at startup, `app/src/gpu/shaders/pipeline_cache.inc`, is optional and also stays out of Git, because it is generated from local play sessions. Without it, the build succeeds; a missing pipeline is created or awaited before its draw. Normal rendering does not skip draws while pipelines compile.

### Output

Main files in `app/out/build/win-amd64-release/`:

```text
narutorise.exe             # with the native renderer, when enabled
narutorise_ai2c.dll
narutorise_ai2c1.dll       # Shikamaru engine (DLC module builds only)
narutorise_ai2c2.dll       # Jiraiya & Sarutobi engine (DLC module builds only)
narutorise_ai2c3.dll       # Choji & Temari engine (DLC module builds only)
rexruntime.dll
rexgpu-xenos.dll
amd_fidelityfx_dx12.dll   # when provided by the SDK
narutorise.toml
shader_cache/              # when packaging/private/shader_cache has the storage
```

## 7. Run

```powershell
.\app\out\build\win-amd64-release\narutorise.exe
```

Select the ISO or `default.xex` from a complete extracted copy on the first-launch screen. Later launches go straight to the game; Shift or `--setup` opens setup. Press F1 or Back+Start for the in-game menu. Nearby `game/` and `game_root/` copies can also be detected.

To launch directly with an explicit path:

```powershell
$gameRoot = (Resolve-Path .\game_root).Path
.\app\out\build\win-amd64-release\narutorise.exe "--game_data_root=$gameRoot"
```

`--game_data_root` also accepts the disc image itself, read in place: `"--game_data_root=D:\Backups\Naruto.iso"`. Code generation still reads the extracted `game_root/` (step 3).

Install the [Visual C++ x64 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe) on the machine running the game. Keep the DLLs alongside the executable. See [configuration](configuration.md) for save, cache, and log locations.

## 8. Create a package

After building:

```powershell
# Portable directory and ZIP (no Inno Setup required)
pwsh -File .\packaging\package-release.ps1

# Additional bridge for old launcher updaters (requires Inno Setup)
pwsh -File .\packaging\package-bridge.ps1
```

The script always refreshes the build before packaging; unchanged sources are not recompiled. `-Build` is still accepted, but changes nothing.

Outputs in `dist/`: `naruto-rise-recomp-win-amd64/`, a ZIP with the same name. The separate bridge command adds `NarutoRiseInstaller.exe` from that payload and its recorded version. Packaging requires `llvm-readobj.exe` to inspect binary dependencies and the Xenos shader storage of the game (`555307E5.xsh` and `555307E5.rtv.d3d12.xpso`) in `packaging/private/shader_cache/`. That folder is ignored by Git because the storage holds the game's shader microcode. The Xenos renderer writes these files to `Documents/narutorise/cache/shaders/shareable/` while you play; copy them from there, or configure the application with `-DNARUTORISE_SHADER_STORAGE_DIR=<folder>` (a CMake cache variable, not an environment variable) to use another folder that has them. When the build includes the native renderer, its shader cache is already inside `narutorise.exe`, and packaging adds the Plume, D3D12 Memory Allocator, and XenosRecomp notices to `licenses/`. See the [packaging guide](../packaging/README.md).

## Configuration and localization regression tests

After configuring the application, verify language persistence, translation coverage, language order, and formatting placeholders without game files or opening the UI:

```powershell
cmake --build app/out/build/win-amd64-release --target narutorise_config_manager_test narutorise_localization_test
$testData = Join-Path $env:TEMP ("narutorise-config-" + [guid]::NewGuid())
.\app\out\build\win-amd64-release\narutorise_config_manager_test.exe $testData
.\app\out\build\win-amd64-release\narutorise_localization_test.exe
```

## Other platforms

Linux, macOS, and ARM64 presets are inherited from the SDK structure. Native file dialogs, shortcut creation, preparation scripts and restart handling still have Windows-specific dependencies. Proton/Game Mode and physical-controller acceptance remain unverified for the new distribution. These presets alone do not guarantee a working build on other platforms.
