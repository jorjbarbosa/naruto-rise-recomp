# Build and run

[Documentation](README.md) · [Configuration](configuration.md) · [DLCs](dlc.md)

This guide describes the project's **Windows x64 / Release** workflow. Run commands from the repository root in PowerShell 7.2+ with the Visual Studio x64 development environment loaded. Stop the sequence if any command fails. The native Linux build has its own [section](#linux).

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
| Inno Setup | 6.4+, only needed to build the installer |

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

The translation loader and launcher use the pinned JSON parser in `third_party/nlohmann_json`, through the `narutorise::json` CMake target. This application dependency does not require the SDK's private inja headers or a separate download.

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

## 6. Build the game and launcher

```powershell
$sdkPrefix = (Resolve-Path .\sdk\out\install\win-amd64).Path
cmake --preset win-amd64-release -S app -B app/out/build/win-amd64-release "-DCMAKE_PREFIX_PATH=$sdkPrefix"
cmake --build app/out/build/win-amd64-release
```

The complete build includes the game, launcher, installer helper, assets, configuration, and shader cache. The extractor is required by the helper target even if you only intend to run the game.

### Native renderer (optional)

The [native renderer](renderers.md) is built into `narutorise.exe` when the application is configured with `NARUTORISE_NATIVE_RENDERER_BOOTSTRAP=ON`. The executable keeps the Xenos plugin, and the launcher lets you choose the renderer ([Renderer](configuration.md#renderer)). It needs:

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

The launcher lists only the native APIs included in that build. `native_backend = "vulkan"` selects Vulkan; an omitted key selects D3D12 when available, otherwise Vulkan. Explicitly requesting an absent API fails without switching APIs. Vulkan needs the system loader and a compatible driver, including extended dynamic state and swapchain maintenance1 (KHR or EXT) for presentation retirement. Users do not need DXC, Vulkan SDK, `spirv-val` or validation layers.

The list of pipelines compiled at startup, `app/src/gpu/shaders/pipeline_cache.inc`, is optional and also stays out of Git, because it is generated from local play sessions. Without it, the build succeeds; a missing pipeline is created or awaited before its draw. Normal rendering does not skip draws while pipelines compile.

### Output

Main files in `app/out/build/win-amd64-release/`:

```text
narutorise.exe             # with the native renderer, when enabled
narutorise_launcher.exe
narutorise_ai2c.dll
narutorise_ai2c1.dll       # Shikamaru engine (DLC module builds only)
narutorise_ai2c2.dll       # Jiraiya & Sarutobi engine (DLC module builds only)
narutorise_ai2c3.dll       # Choji & Temari engine (DLC module builds only)
rexruntime.dll
rexgpu-xenos.dll
amd_fidelityfx_dx12.dll   # when provided by the SDK
narutorise.toml
assets/
shader_cache/              # when packaging/private/shader_cache has the storage
```

## 7. Run

```powershell
.\app\out\build\win-amd64-release\narutorise_launcher.exe
```

Select the folder containing `default.xex`, choose your monitor's aspect ratio, and launch the game. The launcher also searches for `game/` and `game_root/` in known locations, including this checkout's root.

To launch directly with an explicit path:

```powershell
$gameRoot = (Resolve-Path .\game_root).Path
.\app\out\build\win-amd64-release\narutorise.exe "--game_data_root=$gameRoot"
```

Install the [Visual C++ x64 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe) on the machine running the game. Keep the DLLs and assets alongside the executable. See [configuration](configuration.md) for save, cache, and log locations.

## 8. Create a package

After building:

```powershell
# Portable directory and ZIP, without requiring Inno Setup
pwsh -File .\packaging\package-release.ps1 -NoSetup

# Portable directory, ZIP, and installer (requires Inno Setup)
pwsh -File .\packaging\package-release.ps1
```

The script always refreshes the build before packaging; unchanged sources are not recompiled. `-Build` is still accepted, but changes nothing.

Outputs in `dist/`: `naruto-rise-recomp-win-amd64/`, a ZIP with the same name, and, without `-NoSetup`, `NarutoRiseInstaller.exe`. Packaging requires `llvm-readobj.exe` to inspect binary dependencies and the Xenos shader storage of the game (`555307E5.xsh` and `555307E5.rtv.d3d12.xpso`) in `packaging/private/shader_cache/`. That folder is ignored by Git because the storage holds the game's shader microcode. The Xenos renderer writes these files to `Documents/narutorise/cache/shaders/shareable/` while you play; copy them from there, or configure the application with `-DNARUTORISE_SHADER_STORAGE_DIR=<folder>` (a CMake cache variable, not an environment variable) to use another folder that has them. When the build includes the native renderer, its shader cache is already inside `narutorise.exe`, and packaging adds the Plume, D3D12 Memory Allocator, and XenosRecomp notices to `licenses/`. See the [packaging guide](../packaging/README.md).

## Configuration and localization regression tests

After configuring the application, verify language persistence, translation coverage, language order, and formatting placeholders without game files or opening the UI:

```powershell
cmake --build app/out/build/win-amd64-release --target narutorise_config_manager_test narutorise_localization_test
$testData = Join-Path $env:TEMP ("narutorise-config-" + [guid]::NewGuid())
.\app\out\build\win-amd64-release\narutorise_config_manager_test.exe $testData
.\app\out\build\win-amd64-release\narutorise_localization_test.exe
```

## Linux

The game builds natively on Linux x86_64, without Wine or Proton: the native renderer on Vulkan, with the window on Wayland or X11 through SDL3. The Linux build has no launcher, installer, Xenos renderer, or DLC modules yet; settings are edited in `narutorise.toml`. It was tested on Ubuntu 26.04 with Clang 21 and CMake 4.2, building in WSL and running on a desktop with Mesa's RADV.

### Prerequisites

The packages installed on the test system:

```bash
sudo apt install clang cmake ninja-build git pkg-config python3 spirv-tools nasm \
  libssl-dev libgtk-3-dev libsdl2-dev libasound2-dev libpulse-dev libwayland-dev \
  libxkbcommon-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev libxss-dev \
  libudev-dev libvulkan-dev
```

Running the game needs only a Vulkan driver (Mesa, or the NVIDIA driver) and the Vulkan loader, which desktop distributions install by default. DXC for Linux comes with the XenosRecomp submodule, so the shaders are translated and compiled on Linux without another compiler.

### Build

Prepare the game files as in [section 3](#3-prepare-the-game-files), then, from the repository root:

```bash
git submodule update --init --recursive
bash patches/apply_sdk_patches.sh

cmake --preset linux-amd64 -S sdk -B sdk/out/build/linux-amd64
cmake --build sdk/out/build/linux-amd64 --config Release
cmake --install sdk/out/build/linux-amd64 --config Release

git clone https://github.com/renderbag/plume.git out/plume
git -C out/plume checkout d72379344dacd3dbf9f810f92ddc87e6de1845b1
git -C out/plume submodule update --init contrib/volk contrib/Vulkan-Headers contrib/VulkanMemoryAllocator

./sdk/out/install/linux-amd64/bin/rexglue codegen app/narutorise_manifest.toml
cmake --preset linux-amd64-release -S app -B app/out/build/linux-amd64-release \
  -DCMAKE_PREFIX_PATH="$PWD/sdk/out/install/linux-amd64" \
  -DNARUTORISE_NATIVE_RENDERER_BOOTSTRAP=ON \
  -DNATIVE_RENDERER_PLUME_SOURCE="$PWD/out/plume" \
  -DNARUTORISE_GAME_DIR="$PWD/game_root"
cmake --build app/out/build/linux-amd64-release
```

`apply_sdk_patches.sh` is the shell version of `apply_sdk_patches.ps1`: it applies `patches/sdk/` in name order and skips the patches already applied. The native renderer is required on Linux (`NARUTORISE_NATIVE_RENDERER_BOOTSTRAP=ON`) and builds with Vulkan only; Plume needs only its Volk, Vulkan Headers and Vulkan Memory Allocator submodules there. After a change to the installed SDK's CMake files, rerun the application's configure step: the install keeps their source timestamps, so the build does not notice the change.

### Run

`app/out/build/linux-amd64-release/` holds `narutorise`, the SDK runtime `librexruntime.so`, the `libnarutorise_ai2c*.so` modules, and `narutorise.toml`. Copy them together; the executable finds the libraries next to itself.

```bash
./app/out/build/linux-amd64-release/narutorise --game_data_root="$PWD/game_root"
```

Without `--game_data_root`, the game looks for `game/` or `game_root/` next to the executable. Saves go to `~/.local/share/narutorise` (`$XDG_DATA_HOME/narutorise`; `REX_USER_DATA_ROOT` points them elsewhere), and the logs to `logs/` next to the executable. `F11` or `Alt+Enter` toggle fullscreen. SDL picks Wayland or X11; `SDL_VIDEO_DRIVER=x11` forces X11, and the log names the one in use (`SDL video driver`).

### Known limitations

- Bink videos play at about 10 FPS, with stuttering audio. The cause is not established yet.
- The first launch builds the pipelines with a cold driver cache, which took about a minute on RADV; the game is slower meanwhile. Later launches use the driver's disk cache.
- The diagnostics that depend on Windows (minidumps, the stack sampler, heap watch) are not available. The crash report in the log works.

macOS and ARM64 presets are inherited from the SDK structure; they are not tested.
