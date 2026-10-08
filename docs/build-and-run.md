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
├── ai2c2.dll       # optional copy of AI2C@2.dll from the DLC
└── ...            # remaining disc files and folders
```

To extract an ISO using the tool downloaded by setup, use a new destination folder:

```powershell
.\tools\extract-xiso\extract-xiso.exe -x -d .\game_root "D:\Backups\Naruto.iso"
```

The expected Title ID is `555307E5`. Identifying the title does not establish compatibility with every revision; seeds and patches contain revision-specific addresses.

### Choose a build with or without DLC

The current manifest **declares the DLC module** `ai2c2.dll`. Before running code generation:

- **With character DLC:** copy the `AI2C@2.dll` extracted from your package to `game_root/ai2c2.dll`. See [DLCs — recompiled module](dlc.md#33-generate-the-recompiled-module).
- **Base game only:** in your local copy of `app/narutorise_manifest.toml`, remove only the final `[[modules]]` block containing `guest_path = "ai2c@2.dll"` and its fields. Keep the `ai2c.dll` block and entrypoint seeds. Restore the block when preparing a DLC-enabled build.

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

This reads the manifest and `*-seeds.toml` files, producing sources in `app/generated/default/`, `app/generated/ai2c/`, and, when configured, `app/generated/ai2c2/`.

Run initial code generation **before configuring the application**, so CMake can find source lists and DLL targets. Subsequent builds use the `narutorise_codegen` target for incremental regeneration. After adding or removing modules, rerun code generation and CMake configuration.

## 6. Build the game and launcher

```powershell
$sdkPrefix = (Resolve-Path .\sdk\out\install\win-amd64).Path
cmake --preset win-amd64-release -S app -B app/out/build/win-amd64-release "-DCMAKE_PREFIX_PATH=$sdkPrefix"
cmake --build app/out/build/win-amd64-release
```

The complete build includes the game, launcher, installer helper, assets, configuration, and shader cache. The extractor is required by the helper target even if you only intend to run the game.

Main files in `app/out/build/win-amd64-release/`:

```text
narutorise.exe
narutorise_launcher.exe
narutorise_ai2c.dll
narutorise_ai2c2.dll       # DLC module builds only
rexruntime.dll
rexgpu-xenos.dll
amd_fidelityfx_dx12.dll   # when provided by the SDK
narutorise.toml
assets/
shader_cache/
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

# Rebuild and also generate the installer (requires Inno Setup)
pwsh -File .\packaging\package-release.ps1 -Build
```

Outputs in `dist/`: `naruto-rise-recomp-win-amd64/`, a ZIP with the same name, and, when requested, `NarutoRiseInstaller.exe`. Packaging requires the Naruto shader cache and `llvm-readobj.exe` to inspect binary dependencies. See the [packaging guide](../packaging/README.md).

## Configuration and localization regression tests

After configuring the application, verify language persistence, translation coverage, language order, and formatting placeholders without game files or opening the UI:

```powershell
cmake --build app/out/build/win-amd64-release --target narutorise_config_manager_test narutorise_localization_test
$testData = Join-Path $env:TEMP ("narutorise-config-" + [guid]::NewGuid())
.\app\out\build\win-amd64-release\narutorise_config_manager_test.exe $testData
.\app\out\build\win-amd64-release\narutorise_localization_test.exe
```

## Other platforms

Linux, macOS, and ARM64 presets are inherited from the SDK structure. The launcher, preparation scripts, and installer still have Windows-specific dependencies; the launcher's graphics path uses D3D11. These presets alone do not guarantee a working build on other platforms.
