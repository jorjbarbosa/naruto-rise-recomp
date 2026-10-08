<p align="center">
  <img src="docs/assets/naruto-rise-logo.png" alt="Naruto: Rise of a Ninja" width="600">
</p>

<h1 align="center">Naruto: Rise of a Ninja — Recomp</h1>

<p align="center">
  A static recompilation for PC powered by ReXGlue<br>
  <strong>Ultrawide • DLC support • Launcher and installer in seven languages</strong>
</p>

<p align="center">
  <a href="#installing">Installing</a> ·
  <a href="#updating">Updating</a> ·
  <a href="docs/build-and-run.md">Build and run</a> ·
  <a href="docs/configuration.md">Configuration</a> ·
  <a href="docs/dlc.md">DLCs</a> ·
  <a href="docs/troubleshooting.md">Troubleshooting</a>
</p>

## About

A PC port of **Naruto: Rise of a Ninja**, originally released for Xbox 360. The project uses the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) to translate the game's PowerPC code into C++ and compile it for native execution. The SDK runtime provides system services, graphics, audio, and input.

The current build and distribution workflow targets **Windows x64**. The project is under development; presets for other platforms exist, but do not represent complete or verified support for this port.

## Features

- **21:9 and 32:9 ultrawide**, with projection adjustments and anamorphic presentation; a 16:9 option is available in the launcher.
- **DLC support**, with launcher import and STFS package installation at game startup. Character DLCs use an additional recompiled engine module.
- **Game language selection**, with English, French, German, Spanish, and Italian options in the launcher; available text and voices depend on your game files.
- **1× to 4× resolution scaling**, up to 16× anisotropic filtering, fullscreen, and VSync.
- **Post-processing**, including FXAA and optional SDK FidelityFX integration.
- **FPS and frametime overlay**, toggled with `F1`, showing the guest (game) and host frame rates, and an option to skip intro videos.
- **Bundled shader cache**, copied into the user cache when available.
- **Portable distribution and Windows installer**, with optional ISO import and a manual update mode for existing installations.

## Getting started

You need the files from your Xbox 360 copy of the game. The source tree does not include game data or DLC packages.

1. Follow [Installing](#installing) for a Windows release package or use the [build guide](docs/build-and-run.md).
2. Import your ISO through the installer. For a portable installation, place extracted files in `game/` or select their folder in the launcher.
3. Open `narutorise_launcher.exe`, select your monitor's aspect ratio, and adjust graphics settings.
4. Launch the game. For additional content, see the [DLC guide](docs/dlc.md).

Runtime requirements: Windows x64, a Direct3D 12-compatible GPU for the default backend, and the [Microsoft Visual C++ x64 Redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe). The launcher uses Direct3D 11. An SDL-compatible controller is recommended; mouse and keyboard mode is disabled by default.

## Installing

Download `NarutoRiseInstaller.exe` from the **Assets** of the latest Windows release on the [Releases page](https://github.com/jorjbarbosa/naruto-rise-recomp/releases). The installer includes the PC port; you must provide the original Xbox 360 game files separately.

1. Run `NarutoRiseInstaller.exe` and choose the installer language.
2. Select **Install a new copy or import an ISO**.
3. Choose the destination folder. The default is `%LOCALAPPDATA%\NarutoRisePC`, a per-user installation that does not require administrator privileges.
4. Select the ISO from your Xbox 360 copy to extract it into `game/`, or leave the ISO field empty to use files you already extracted.
5. Choose the shortcuts, review the summary, and install.
6. Open `narutorise_launcher.exe`, select your monitor's aspect ratio, adjust settings, and launch the game.

If you skipped ISO import, place your extracted files in `game/` next to the launcher or select their folder in the launcher's **Game Files** tab. The selected game folder must contain `default.xex`. To import an ISO later, run the installer again and select **Install a new copy or import an ISO**; the destination's `game/` folder must be empty.

For the portable ZIP, extract it to a writable folder and run `narutorise_launcher.exe`. Place or select the extracted game files as described above. ISO import is available through the installer.

## Updating

Updates are installed manually with `NarutoRiseInstaller.exe` from a newer Windows release on the [Releases page](https://github.com/jorjbarbosa/naruto-rise-recomp/releases/latest). Use the same installer for a new installation or to update an existing copy.

1. Close the game and launcher.
2. Run the new `NarutoRiseInstaller.exe` and choose the installer language.
3. Select **Update an existing installation**. This option is preselected when an installation is detected in the initial destination.
4. Confirm the existing installation folder: it must contain both `narutorise.exe` and `narutorise_launcher.exe`. Select the PC port folder, rather than the folder containing only your Xbox 360 game files.
5. Review the destination and versions, apply the update, and reopen the launcher when it finishes.

The installer detects previous Inno Setup and legacy native installations, including custom destinations. For a portable copy, select its folder manually; updating it through the installer registers it as an installed Windows application and adds an uninstaller.

Updates replace the PC port binaries and bundled assets while preserving your game files, `narutorise.toml`, language preferences, saves, DLCs, and personal files. You do not need to uninstall the previous version or import your ISO again; the ISO page is skipped during an update. Existing `game/` and legacy `game_root/` folders are preserved.

See the [packaging guide](packaging/README.md) for installer details and command-line options.

## Documentation

| Guide | Contents |
| --- | --- |
| [Build and run](docs/build-and-run.md) | Toolchain, SDK, code generation, compilation, and first launch |
| [Configuration](docs/configuration.md) | Ultrawide, graphics, shortcuts, and user files |
| [DLCs and Title Updates](docs/dlc.md) | Content installation and additional module build |
| [Architecture and analysis](docs/architecture.md) | Project structure, technical workflow, and areas to address |
| [Troubleshooting](docs/troubleshooting.md) | Build failures, runtime issues, and diagnostics |
| [Packaging](packaging/README.md) | Portable ZIP and Inno Setup installer |
| [Installer verification](docs/installer-status.md) | Existing tests and manual verification steps |

## Contributing

Read the [technical analysis](docs/architecture.md) before changing the runtime or code generation files. When reporting an issue, include reproduction steps, version/build, CPU, GPU, driver, configuration, and logs from `logs/` next to the executable. Also mention whether you use ultrawide, DLCs, or a Title Update.

Keep game files, content packages, and generated recompiled code out of commits. SDK changes should include reproducible patches in `patches/sdk/`.

## Credits

- [ReXGlue](https://github.com/rexglue/rexglue-sdk), recompilation SDK and runtime.
- [Xenia](https://github.com/xenia-project/xenia) and [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), foundations and references for the Xbox 360 recompilation ecosystem.
- [SDL](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui), [extract-xiso](https://github.com/XboxDev/extract-xiso), and [Inno Setup](https://jrsoftware.org/isinfo.php).
- The Xenia Canary community and Hells Gate Recomp, references cited in the ultrawide implementation.
- Ubisoft Montreal, for the original game. [Logo source](docs/assets/README.md).

Dependency licenses are located in their respective directories.
