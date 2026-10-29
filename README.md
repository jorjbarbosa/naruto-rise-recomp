> [!TIP]
> Looking for Naruto: The Broken Bond? Click [here](https://github.com/jorjbarbosa/broken-bond-recomp/releases/latest)

<p align="center">
  <img src="docs/assets/naruto-rise-logo.png" alt="Naruto: Rise of a Ninja" width="600">
</p>

<h1 align="center">Naruto: Rise of a Ninja — Recomp</h1>

<p align="center">
  A static recompilation for PC powered by ReXGlue<br>
  <strong>Native Direct3D 12 renderer with 60 and 120 FPS • Ultrawide • DLC support • Seven languages</strong>
</p>

<p align="center">
  <a href="#installing">Installing</a> ·
  <a href="#updating">Updating</a> ·
  <a href="#native-renderer-and-60-fps">Native renderer and 60 FPS</a> ·
  <a href="docs/build-and-run.md">Build and run</a> ·
  <a href="docs/configuration.md">Configuration</a> ·
  <a href="docs/troubleshooting.md">Troubleshooting</a>
</p>

> [!WARNING]
> This project is a work in progress. Expect bugs, crashes, and visual issues; the native renderer, now the default, is the newest part, and Xenos remains available through manual configuration if something does not look right. Keep a backup of your saves and report problems on the [Issues page](https://github.com/jorjbarbosa/naruto-rise-recomp/issues).

## About

A PC port of **Naruto: Rise of a Ninja**, originally released for Xbox 360. The project uses the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) to translate the game's PowerPC code into C++ and compile it for native execution. The SDK runtime provides system services, audio, and input.

The game can be drawn by two renderers, available in the executable: the **native renderer**, the default, which draws the game's Direct3D calls with Direct3D 12 or Vulkan and can present at 60 or 120 FPS, and **Xenos**, the SDK's emulation of the Xbox 360 GPU, as the alternative.

The current build and distribution workflow targets **Windows x64**. Presets for other platforms exist, but do not represent complete or verified support for this port.

> [!NOTE]
> AI tools are used to assist the development of this project. The project's direction, technical decisions, and testing are the maintainer's, who reviews every change before it is published.

## Features

**Graphics**

- **Two renderers:** the native Direct3D 12 renderer (default) and Xenos as the alternative. See [Native renderer and 60 FPS](#native-renderer-and-60-fps).
- **60 and 120 FPS** with the native renderer, keeping the game's logic at its original rate.
- **21:9 and 32:9 ultrawide** with the Xenos renderer, with projection adjustments and anamorphic presentation; configured through `narutorise.toml`.
- **Internal resolution from 1× to 4×** (also 1.5× with the native renderer), up to 16× anisotropic filtering, FXAA, FidelityFX CAS/FSR, fullscreen, and VSync.
- **Native renderer options:** MSAA (2×, 4×, 8×), transparency antialiasing, brightness, frame latency, and triple buffering.
- **Graphics card selection** for both renderers; hybrid laptops use the dedicated card by default.

**Content and languages**

- **DLC support**, with import from the setup screen or in-game menu and STFS package installation at game startup. Character DLCs use additional recompiled engine modules.
- **Game language selection**, with English, French, German, Spanish, and Italian options in Settings → Game; available text and voices depend on your game files.
- **External translation mods**, selected in the menu's game language list, with UTF-8 text replacements by ID and fallback to the original English text. Releases include the complete Brazilian Portuguese package. See the [translation mod guide](docs/translation-mods.md).

**Menus and distribution**

- **Open the game directly:** extract the ZIP, run `narutorise.exe`, and select your original Xbox 360 ISO or a complete extracted copy. The ISO is read in place.
- **Setup screen** on first launch, for missing game files, or in safe mode (hold Shift while opening or use `--setup`).
- **In-game menu** with Settings, DLC and Exit game: press `F1` or `Back+Start`. Both shortcuts are remappable. The game continues running while menu input is captured.
- **Seven interface languages**, shared settings before and during the game, Show FPS, shader progress, update notices and desktop shortcut creation.
- **ZIP distribution**, plus a temporary compatibility bridge for older installed copies.
- **Shader caches:** bundled Xenos storage seeds the user cache; native shaders are translated at build time and embedded in the executable.

## Native renderer and 60 FPS

On Xbox 360, the game runs its logic in fixed steps: 30 per second in the open world and 60 in fights. The Xenos renderer keeps those rates. The native renderer can present more frames than the logic produces:

| | Xenos | Native (default) |
| --- | --- | --- |
| How it draws | Emulates the Xbox 360 GPU and the command buffers the game writes for it | Intercepts the game's Direct3D calls and issues them with Direct3D 12 or Vulkan; the shaders are translated when the port is built |
| Frame rate | The game's: 30, and 60 in fights | 30, 60 or 120 in Settings (up to 240 in `narutorise.toml`) |
| Aspect ratio | 16:9, 21:9, and 32:9 | 16:9, with bars on wider monitors |
| Options only in this renderer | Aspect ratio, dithering, `F1`/`F3` overlays | MSAA, transparency antialiasing, frame rate, brightness, frame latency, triple buffering |

The native renderer is the default: choose Direct3D 12 or Vulkan and the **Frame rate** under **Settings → Display** (60 by default). Xenos is available by setting `renderer = "xenos"` in `narutorise.toml`; its path does not provide the native setup or in-game menu. Builds without the native renderer use Xenos ([build guide](docs/build-and-run.md#native-renderer-optional)).

Above 30 FPS, the game's logic keeps its original step, and the renderer draws the frames in between by interpolating objects, characters' poses, and the camera between the last two logic steps. Game speed, physics, and timing stay those of the console. With VSync on, the rate does not exceed the monitor's refresh rate. The HUD minimap still updates at 30 FPS, and the image trails the logic by one step (up to about 33 ms in the open world).

See [Renderers and frame rate](docs/renderers.md) for how it works, its limits, and its known issues, and [Configuration](docs/configuration.md#renderer) for every option.

## Requirements

- Windows x64.
- A GPU and driver compatible with Direct3D 12 or the native Vulkan backend (Xenos requires Direct3D 12).
- The [Microsoft Visual C++ x64 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- Your Xbox 360 ISO or a complete extracted copy, including `default.xex`. The source tree does not include game data or DLC packages.
- An SDL-compatible controller is recommended. Keyboard and mouse can be configured in Settings → Controls; see [Keyboard and mouse](docs/configuration.md#keyboard-and-mouse).

## Installing

Download `naruto-rise-recomp-win-amd64.zip` from the [Releases page](https://github.com/jorjbarbosa/naruto-rise-recomp/releases). Extract it to a writable folder and run `narutorise.exe`. Game data is supplied separately. The ZIP creates `data/` for preferences, logs, saves and DLC packages; `defaults/narutorise.toml` is the factory template. Settings → Advanced can show, open or change the data folder after restart. Linux AppImage uses the XDG data directory by default. See [Data folder and recovery](docs/data-folder.md).

On first launch, choose your original Xbox 360 ISO or `default.xex` from a complete extracted copy. The game validates the supported revision and reads the ISO directly. Later launches follow the Show menu at startup preference. Hold **Shift** while opening, or run `narutorise.exe --setup`, to return to setup.

Press **F1** or **Back+Start** while playing for Settings, DLC and Exit game. Create a desktop shortcut from **Settings → Advanced**. See the [DLC guide](docs/dlc.md) for additional content.

## Updating

The game checks GitHub Releases in the background and offers to open the release page. It does not download or install updates. Release eligibility follows GitHub's published/pre-release flag; a `beta` tag can still be eligible. Checks are cached; playing offline works.

Close the game, download the new ZIP, and replace the package files while **keeping the active `data/` folder and `data_location.json` selection file**. Preserve `game/`, `game_root/`, saves, DLC and mods. All active mods and translations live in the active data folder's `mods/`; eligible old mods are copied there with originals preserved. Updates refresh only the immutable `defaults/mods/pt-br` seed and preserve active personalizations. To update an active translation, back it up and merge the new seed deliberately. See [data-folder recovery](docs/data-folder.md).

For an installed **0.3.0-beta** copy, use the separately attached `NarutoRiseInstaller.exe` compatibility bridge. The old launcher's updater can hand off to it using `/MODE=update`. It keeps the installation folder and user data, replaces the known shortcuts to open `narutorise.exe`, and removes the old launcher. You can also run the bridge manually, choose **Update an existing installation** and select the PC port folder. There is no ISO import step.

The bridge remains attached for several releases because older launchers check only the latest release. For a portable ZIP updated manually, the old `narutorise_launcher.exe` is left untouched; you may delete it. See the [packaging guide](packaging/README.md) for commands and migration tests.

## Documentation

| Guide | Contents |
| --- | --- |
| [Build and run](docs/build-and-run.md) | Toolchain, SDK, code generation, compilation, the native renderer, and first launch |
| [Configuration](docs/configuration.md) | Ultrawide, renderers, graphics, shortcuts, and user files |
| [Renderers and frame rate](docs/renderers.md) | Xenos and native renderers, 60 and 120 FPS, and known issues |
| [DLCs and Title Updates](docs/dlc.md) | Content installation and additional module build |
| [Architecture and analysis](docs/architecture.md) | Project structure, technical workflow, and areas to address |
| [Troubleshooting](docs/troubleshooting.md) | Build failures, runtime issues, and diagnostics |
| [Packaging](packaging/README.md) | ZIP distribution and compatibility bridge |

## Roadmap

Planned work, in no particular order:

- **Linux support.** Build presets exist, but the port has not been validated on Linux, and native file dialogs and restart behavior still need platform integration.
- **Ultrawide with the native renderer.** It currently draws at 16:9, with bars on wider monitors.

## Contributing

Read the [technical analysis](docs/architecture.md) before changing the runtime or code generation files. When reporting an issue, include reproduction steps, version/build, CPU, GPU, driver, the renderer and frame rate you use, your configuration, and logs from `logs/` inside the active data folder shown in Settings → Advanced. Also mention whether you use ultrawide, DLCs, or a Title Update.

Keep game files, content packages, derived shader data, and generated recompiled code out of commits. SDK changes should include reproducible patches in `patches/sdk/`.

## Credits

**Testing**

- [entityeternal](https://github.com/entityeternal), for testing the native renderer and frame rate builds and reporting issues.

**Projects and libraries**

- [ReXGlue](https://github.com/rexglue/rexglue-sdk), recompilation SDK and runtime.
- [Xenia](https://github.com/xenia-project/xenia) and [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), foundations and references for the Xbox 360 recompilation ecosystem.
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp), shader translation for the native renderer (forked in `tools/xenosrecomp/`), and [Plume](https://github.com/renderbag/plume) with [D3D12 Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator), its Direct3D 12 backend.
- [SDL](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui), [AMD FidelityFX](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK), [extract-xiso](https://github.com/XboxDev/extract-xiso), and [Inno Setup](https://jrsoftware.org/isinfo.php).

**References**

- [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp), the model for the native renderer's design and its graphics card option.
- The Xenia Canary community and Hells Gate Recomp, references cited in the ultrawide implementation.
- Ubisoft Montreal, for the original game. [Logo source](docs/assets/README.md).

## License

Copyright (C) 2026 Jorge Barbosa. The source code of this port is licensed under version 3 of the [GNU General Public License](LICENSE) (GPL-3.0-only). The v0.2.0-beta.1 release was published under the MIT License.

Third-party code keeps its own license: the ReXGlue SDK (`sdk/`, BSD-3-Clause) and its dependencies, the XenosRecomp fork (`tools/xenosrecomp/`, MIT), Plume and D3D12 Memory Allocator (MIT), RmlUi (MIT), FreeType (FTL), and the embedded Roboto Condensed fonts (`app/ui/fonts/`, OFL-1.1). The developer extractor remains under its own license in `tools/extract-xiso/`. Release packages include the notices of every component they ship in `licenses/` ([component list](packaging/third-party-licenses.psd1)).

Naruto: Rise of a Ninja, its content, and its artwork and trademarks, including the game artwork in `launcher/assets/` and `docs/assets/`, belong to their respective owners and are not covered by this license. No game data is distributed with this project.
