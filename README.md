> [!TIP]
> Looking for Naruto: The Broken Bond? Click [here](https://github.com/jorjbarbosa/broken-bond-recomp/releases/latest)

> [!IMPORTANT]
> This is the port rebuilt on [XenonRecomp](https://github.com/hedge-dev/XenonRecomp). It has no release yet.
> To play now, download the [latest release](https://github.com/jorjbarbosa/naruto-rise-recomp/releases/latest)
> of the previous port, built on ReXGlue.

<h1 align="center">Naruto: Rise of a Ninja — Recomp</h1>

<p align="center">
  A static recompilation of the Xbox 360 game for PC<br>
  <strong>Direct3D 12 and Vulkan • Up to 4× internal resolution • Ultrawide up to 32:9 • 60 FPS and beyond</strong>
</p>

<p align="center">
  <a href="#features">Features</a> ·
  <a href="#building">Building</a> ·
  <a href="#running">Running</a> ·
  <a href="#settings">Settings</a> ·
  <a href="#status">Status</a> ·
  <a href="#support">Support</a>
</p>

<p align="center">
  <a href="https://github.com/jorjbarbosa/naruto-rise-recomp/stargazers"><img src="https://img.shields.io/github/stars/jorjbarbosa/naruto-rise-recomp" alt="Stars"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/jorjbarbosa/naruto-rise-recomp" alt="License"></a>
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20(experimental)-blue" alt="Platforms">
  <a href="https://buymeacoffee.com/jorjbarbosa"><img src="https://img.shields.io/badge/Buy%20Me%20a%20Coffee-support-FFDD00?logo=buymeacoffee&logoColor=black" alt="Buy Me a Coffee"></a>
</p>

> [!WARNING]
> This port is a work in progress and is only built from source for now. Expect bugs, crashes and
> visual issues, keep a backup of your saves, and report problems on the
> [Issues page](https://github.com/jorjbarbosa/naruto-rise-recomp/issues).

## About

A PC port of **Naruto: Rise of a Ninja** (Ubisoft Montreal, Xbox 360). [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)
translates the game's PowerPC code (`default.xex` and `AI2C.dll`) into C++, which is compiled for x64 and
runs on a runtime written for this game: memory, kernel and XAM services, files, audio with an XMA decoder,
input, and a renderer. The renderer draws the game's Direct3D calls with Direct3D 12 or Vulkan, through
[Plume](https://github.com/renderbag/plume); [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) translates
the game's shaders when the port is built.

The repository contains no game code or data. The translation is made from your own copy of the game, and
the game's files are read from it at run time.

This code base replaces the previous port, built on the ReXGlue SDK (v0.4.2-beta and earlier), whose
releases stay available until this one has its own.

> [!NOTE]
> AI tools are used to assist the development of this project. The project's direction, technical
> decisions, and testing are the maintainer's, who reviews every change before it is published.

## Features

**Graphics**

- **Direct3D 12** (default on Windows) or **Vulkan** (`ROAN_GPU_API=vulkan`, and the only one on Linux).
- **Internal resolution** from 1× to 4× of the game's 1280×720 (fractions allowed), or **auto**: the size the
  picture is shown at in the window. The picture is reduced to the window by area averaging.
- **Ultrawide up to 32:9**: a window wider than 16:9 is filled at its own width, with the camera widened and
  the menus, dialogues, videos and HUD kept in shape (or 16:9 with bars, if you prefer).
- **Anti-aliasing**: the game's own 2× and 4× MSAA, or off, 2×, 4× or 8× in its place; **transparency
  anti-aliasing** for the cut-out edges of leaves, grass and hair.
- **Anisotropic filtering** up to 16× on the game's textures.
- **Interface edges at the internal resolution**: the menus' vector shapes are drawn at the rendering size.
- **Image fit**: aspect, stretch, or integer scaling.
- **Frame rate**: the game's own, the monitor's refresh rate, or any rate from 31 to 240 FPS. The game's logic
  keeps its original step (30 per second in the open world, 60 in fights); the frames in between are drawn by
  interpolating objects, poses and the camera, so game speed and timing stay those of the console.
- **Windowed or borderless**, switched with `Alt+Enter`; the window's size and place are kept.

**Port menu**

- Opened with `Escape`, the controller's **Guide** button, or from the game's own **Options**. The game is
  paused while it is open.
- Pages **GAME**, **VIDEO**, **AUDIO** and **CONTROLS**; changes apply while playing, except the graphics card
  and the game language, which apply at the next start.
- Keyboard bindings: one or two keys per controller input, rebound in the CONTROLS page.

**Game and system**

- **Game language**: English, French (text, voices and videos), Spanish, Italian and German (text).
- **Game files** from the extracted folder, the path to its `default.xex`, the Xbox 360 disc image (read in
  place), or a ZArchive (`.zar`).
- **Saves** kept beside the executable, in `data/user/`.
- **Audio**: master volume, output device, and mute while the window is in the background.

## Requirements

- **Windows x64** with a GPU and driver that support Direct3D 12 or Vulkan.
- **Linux x86-64** (experimental, see [Status](#status)) with a Vulkan driver and **Linux 6.7 or newer**.
- Your own copy of the Xbox 360 game: its disc image, or the extracted files with `default.xex`,
  `AI2C.dll` and `xeshader.sdb`.
- A controller is recommended; the keyboard works with the default bindings below.

## Building

### Windows

- Visual Studio 2022 (or its Build Tools) with **C++ Clang tools for Windows** (clang-cl 19), CMake and Ninja.
  The scripts load the developer environment themselves.
- Git, with the submodules.

```powershell
git clone --recursive https://github.com/jorjbarbosa/naruto-rise-recomp.git
cd naruto-rise-recomp
# Put the extracted game files in game/ (default.xex, AI2C.dll, xeshader.sdb and the rest).
.\scripts\recompile.ps1   # applies patches/, builds the recompilers, translates the game into build/ppc and build/shaders
.\scripts\build.ps1       # builds build/runtime/roan-xenon.exe
```

`recompile.ps1` needs the extracted `default.xex`, `AI2C.dll` and `xeshader.sdb` in `game/`, and fails on any
warning from the recompilers. Run it again after a change to `config/` or `patches/`; a change under `src/`
only needs `build.ps1`. The first build compiles the whole generated code and takes a few minutes.

### Linux

The same steps with the shell scripts, using the system's clang, CMake and Ninja:

```sh
./patches/apply.sh
./scripts/recompile.sh
./scripts/build.sh        # builds build/runtime/roan-xenon
```

## Running

```powershell
.\build\runtime\roan-xenon.exe [game]
```

`game` is the game folder, its `default.xex`, the disc image or a ZArchive. Without it, the `game/` folder of
the repository is used. The settings and saves go to `data/user/` beside the executable.

| Action | Keyboard | Controller |
| --- | --- | --- |
| Port menu | `Escape` | Guide |
| Windowed / borderless | `Alt+Enter` | |
| A, B, X, Y | `Space`, `Left Shift`, `F`, `R` | |
| Start, Back | `Enter`, `Backspace` | |
| LB, RB, LT, RT | `Q`, `E`, `Z`, `C` | |
| Left stick, right stick | `W` `A` `S` `D`, `I` `J` `K` `L` | |
| Stick clicks (LS, RS) | `Tab`, `O` | |
| D-pad | Arrow keys | |

## Settings

The port's settings live in `data/user/settings.toml`, written with comments on its first run. Most are also
in the port menu. An environment variable overrides a setting for one run without writing it to the file.

| Setting | Variable | Values (default first) |
| --- | --- | --- |
| `video.resolution_scale` | `ROAN_RESOLUTION_SCALE` | `1` to `4` (fractions allowed), `"auto"` |
| `video.msaa` | `ROAN_MSAA` | `"game"`, `1` (off), `2`, `4`, `8` |
| `video.anisotropy` | `ROAN_ANISOTROPY` | `"game"`, `1` (off), `2`, `4`, `8`, `16` |
| `video.transparency_aa` | `ROAN_TRANSPARENCY_AA` | `false`, `true` |
| `video.ultrawide` | `ROAN_ULTRAWIDE` | `true`, `false` |
| `video.image_fit` | `ROAN_IMAGE_FIT` | `aspect`, `stretch`, `integer` |
| `video.frame_rate` | `ROAN_FRAME_RATE` | `"game"`, `"display"`, `31` to `240` |
| `video.vsync` | `ROAN_VSYNC` | `true`, `false` |
| `video.adapter` | `ROAN_ADAPTER` | `""` (the default card), or a card's name as the log shows it |
| `window.mode` | | `windowed`, `borderless` |
| `audio.volume` | | `100`, down to `0` |
| `audio.device` | | `""` (the system's default), or a device's name as the log shows it |
| `audio.mute_in_background` | | `false`, `true` |
| `gamepad.dead_zone` | | `0` to `50` percent |
| `game.language` | `ROAN_LANGUAGE` | `english`, `french`, `spanish`, `italian`, `german` |
| `keyboard.*` | | one or two SDL key names per controller input |

For development, `ROAN_GPU_API` (`d3d12`, `vulkan`, `none`), `ROAN_GPU_DEBUG=1` (the D3D12 debug layer or the
Vulkan validation layer) and `ROAN_TRACE=1` (kernel call tracing) are read from the environment only.
`scripts/compare-gpu.ps1` replays recorded frames on both APIs and compares the images, and
`scripts/profile.ps1` samples the running game.

## Status

The game boots, plays its videos, and runs its menus, the open world and fights on Direct3D 12 and Vulkan on
Windows. Known gaps:

- **Linux** builds and runs, but has only been tested without a renderer so far; Vulkan on a native Linux
  install, controllers and audio there are next.
- **Vulkan** has only been tested on AMD's driver.
- **Ultrawide**: some HUD elements of the open world stay centered instead of at the screen's edges, and a
  save's thumbnail taken in a wide window is squeezed.
- **Controller bindings** cannot be changed yet; only the keyboard's.
- With vsync off, the process may not exit after the window is closed.
- There is no first-run setup screen yet: the game files are passed on the command line or placed in `game/`.

## Contributing

When reporting an issue, include the steps to reproduce it, your CPU, GPU and driver, the API (Direct3D 12 or
Vulkan), your `settings.toml`, and the log.

Keep game files, derived shader data and the generated code (`build/`) out of commits. Changes to the
submodules (XenonRecomp, XenosRecomp, Plume, SDL and the others) go in `patches/<submodule>/` as numbered
patch files, documented in [`patches/README.md`](patches/README.md), never as commits inside the submodule.

## Support

If you enjoy the project, you can [buy me a coffee](https://buymeacoffee.com/jorjbarbosa). It's optional and
unlocks nothing; releases stay free for everyone.

## Credits

**Testing**

- [entityeternal](https://github.com/entityeternal), for testing the previous port's renderer and frame rate
  builds and reporting issues.
- [JellySnek](https://github.com/JellySnek), for helping the project since the beginning, reporting bugs and
  suggesting features.
- [tylerl567](https://github.com/tylerl567), for testing on the Steam Deck.
- [rubenlopezgirao](https://github.com/rubenlopezgirao), for helping investigate crashes
  ([#72](https://github.com/jorjbarbosa/naruto-rise-recomp/issues/72)).

**Projects and libraries**

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and [XenosRecomp](https://github.com/hedge-dev/XenosRecomp),
  the code and shader recompilers.
- [Plume](https://github.com/renderbag/plume), with [D3D12 Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator)
  and [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator).
- [SDL](https://github.com/libsdl-org/SDL), [FFmpeg](https://ffmpeg.org) (the XMA decoder),
  [RmlUi](https://github.com/mikke89/RmlUi), [FreeType](https://freetype.org), [zlib](https://github.com/madler/zlib),
  [zstd](https://github.com/facebook/zstd) and [toml++](https://github.com/marzer/tomlplusplus).
- The Edo SZ font by Vic Fieger, used by the port menu.

**References**

- [Xenia](https://github.com/xenia-project/xenia) and the Xenia Canary community, whose game patch for this
  title showed where the game keeps its screen ratio.
- [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp), the model for the renderer's design.
- [ReXGlue](https://github.com/rexglue/rexglue-sdk), on which the previous port was built.
- Ubisoft Montreal, for the original game.

## License

Copyright (C) 2026 Jorge Barbosa. The source code of this port is licensed under version 3 of the GNU General
Public License ([GPL-3.0-only](LICENSE)).

Third-party code keeps its own license: the submodules under `tools/` and `thirdparty/` carry theirs, and the
Edo SZ font is covered by `data/ui/fonts/Vic Fieger License.txt`.

Naruto: Rise of a Ninja, its content, artwork and trademarks belong to their respective owners and are not
covered by this license. No game data is distributed with this project.
