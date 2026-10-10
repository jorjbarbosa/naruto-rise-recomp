# Naruto: Rise of a Ninja - Packaging & Installer Guide

This directory contains the packaging and installer configurations for the *Naruto: Rise of a Ninja* PC recompilation port.

See also: [Build and run](../docs/build-and-run.md), [DLCs](../docs/dlc.md), and [installer verification](../docs/installer-status.md).

## Files

- **`package-release.ps1`**: Main PowerShell script to assemble and package the release distribution.
- **`installer.iss`**: [Inno Setup](https://jrsoftware.org/isdl.php) script to build the standard Windows setup wizard (`NarutoRiseInstaller.exe`).
- **`install.ps1`**: Standalone helper script for portable zip users to generate Desktop and Start Menu shortcuts with one click.
- **`version.iss`**: Default release version; override using `-Version` without modifying source.
- **`tests/`**: Synthetic ISO helper tests and isolated Inno install/uninstall tests.

---

## 1. Creating the Redistributable Package (`dist`)

Run from the repository root in PowerShell:
```powershell
powershell -ExecutionPolicy Bypass -File packaging/package-release.ps1
```
To force a rebuild prior to packaging:
```powershell
powershell -ExecutionPolicy Bypass -File packaging/package-release.ps1 -Build
```

Output generated in `dist/`:
- `dist/naruto-rise-recomp-win-amd64/` (portable release directory)
- `dist/naruto-rise-recomp-win-amd64.zip` (compressed zip file)
- `dist/NarutoRiseInstaller.exe` (Inno Setup installer)

Packaging validates tools and inputs first, stages new artifacts in a private
directory, and preserves previous artifacts under `dist/previous-release-*/`.
Existing distribution folders are never recursively deleted. Use
`-OutputRootDir <folder>` to build isolated test releases and `-NoZip` to skip ZIP
generation. The game, generated DLC modules, launcher, and ISO helper are
refreshed automatically so the host registry stays synchronized with its DLLs;
unchanged sources are not recompiled. `-Build` remains accepted for compatibility.
Review backups before removing them manually.

---

## 2. Compiling the Windows Installer (`NarutoRiseInstaller.exe`)

1. Install [Inno Setup 6.4+](https://jrsoftware.org/isdl.php).
2. If Inno Setup is in your PATH or at default installation paths, `package-release.ps1` will automatically invoke `ISCC.exe`.
3. Alternatively, open `packaging/installer.iss` in the Inno Setup Compiler and compile (Ctrl+F9).
4. The output installer will be saved to `dist/NarutoRiseInstaller.exe`.

The setup installs per-user into `%LOCALAPPDATA%\NarutoRisePC`, without elevation.
Its first page offers **Update an existing installation** or **Install a new copy
or import an ISO**. The installation flow is destination, optional game ISO,
shortcuts, ready summary, installation, and completion with an optional launcher
start. Updates skip ISO import. ISO extraction is staged in the
setup's private temporary folder; populated `game` folders and reparse
points are never replaced. Empty folders can receive an ISO import.
Game data and `narutorise.toml` are preserved on uninstall and configuration is
not overwritten on reinstall. A fresh configuration inherits the setup language
for the launcher interface. Both interfaces offer English, Brazilian Portuguese,
French, German, Spanish, Italian, and Russian, in that order. Setup defaults to
English regardless of Windows or a previous installation; numbered choices keep
this order in Inno Setup's language dialog. Portuguese and Russian affect the
interfaces only, while the game's language setting remains independent.
The native `narutorise_setup_helper.exe` validates the root XEX Title ID
(`555307E5`) before extraction, lists files using `extract-xiso -l -s`, and
reports file-count progress. This identifies the title, not dump authenticity
or compatibility of every game revision. The extraction page has a Cancel button;
canceled/failed temporary data is discarded. Copying first targets a unique
private folder on the installation volume; only a complete import is published
as `game` by directory rename. Aborted staging data is cleaned up.
Both drives are conservatively checked for two copies of game data, the port,
and a 256 MB safety margin. Full disc and trimmed XISO layouts are supported.
Detailed extractor output is written to the Inno setup log (`SetupLogging=yes`).

Existing user data is never removed to make room for an import. A forceful process
kill or power loss can leave a private `.narutorise-import-*` staging folder;
inspect such folders before manually cleaning them up. A normal cancel/error
does not publish partial game data.

When replacing a native-wizard installation at the same location, the old ARP
registration is removed after success, without running its unsafe uninstaller.
Old files/shortcuts are not deleted automatically. Installations at other
locations are left alone. Do not manually run the old native uninstaller against
a folder now managed by Inno Setup.

The supplied icon lives in `launcher/assets/narutorise.ico` and is embedded in
the launcher and setup, and used by setup shortcuts.

Like Dante's Inferno, ISO extraction belongs only to Setup. The helper and
`extract-xiso.exe` are unpacked into the setup's private temp directory, never
installed next to the launcher. The launcher only selects/opens extracted game
folders. Run Setup again to import an ISO into an empty `game` folder.
The portable ZIP likewise contains no ISO tools. Existing legacy `game_root`
folders remain read-only discovery fallbacks; they are never renamed automatically.
Technical config keys (`custom_game_root`, `--game_data_root`) stay compatible.

Packaging forces CMake Release mode and audits PE imports to reject debug CRTs.
The setup helper uses a static C++ runtime; the game/launcher require the
Microsoft Visual C++ 2015-2022 Redistributable (x64), as shown in README.txt.

### DLC engine modules

Character packs ship distinct engine revisions: `AI2C@1.dll` (Shikamaru),
`AI2C@2.dll` (Jiraiya & Sarutobi), and `AI2C@3.dll` (Choji & Temari).
Copy them to `game_root/ai2c1.dll`, `ai2c2.dll`, and `ai2c3.dll` before codegen
(see `docs/dlc.md`). Packaging builds each generated revision and includes
`narutorise_ai2c1.dll`, `narutorise_ai2c2.dll`, and `narutorise_ai2c3.dll` in
both portable and installer distributions. A generated revision with a
missing or empty binary fails packaging. Base-game builds may omit all three;
such builds do not support character DLCs. End users install DLCs through
the launcher's **DLC** tab; the game performs the STFS installation on the
next launch.

Use `-IsccPath 'C:\path\ISCC.exe'` for a custom compiler location or `-NoSetup`
to generate only the portable package. The old ImGui installer is not shipped;
its source remains available behind `NARUTORISE_BUILD_LEGACY_INSTALLER=ON`.

### Manual updates (first update: 1.0.1)

Distribute the new `NarutoRiseInstaller.exe` to existing users. They do not need
an updater in the launcher or the original ISO to update the PC port:

1. Close the game and launcher, then run the new installer.
2. Choose **Update an existing installation** (preselected when the initial
   destination contains both executables).
3. Confirm the folder containing `narutorise.exe` and `narutorise_launcher.exe`.
4. Review the destination and version, then apply the update.

Setup detects its own previous installation and also the old native installer's
registered `InstallLocation`, including custom destinations. For an unregistered
portable copy, select its folder manually; updating it registers the copy with
Inno Setup and creates an uninstaller. The folder picker uses the selected folder
directly, without appending a new application subfolder.

Updates replace only the packaged port binaries, DLLs, assets, bundled shader
cache and README. Existing `narutorise.toml`, `game/`, legacy `game_root/`, pending
`dlc/` packages, personal files, and saves/installed DLCs in the configured user
data directory are preserved. No configuration defaults are forcibly migrated.
The previous native uninstaller is never executed. Its obsolete registration is
removed only after a successful update at the same destination, as described above.

An update requires both port executables in the selected folder and refuses an
ISO import. Installed versions are read from `narutorise.version`, or from the
matching Inno/legacy registration for older releases. Installations without
version metadata can still be updated. A newer numeric release version prevents
replacement by an older installer; reinstalling the same version is allowed.
The version marker is also included in portable packages and is removed by the
Inno uninstaller. Keep the same production AppId for every release.

Build the first update with:

```powershell
pwsh -File packaging/package-release.ps1 -Build -Version 1.0.1
```

For automation, the installer accepts `/MODE=update` or `/MODE=install`; when
omitted, it chooses based on the initial destination. Update example:

```powershell
.\NarutoRiseInstaller.exe /MODE=update /DIR="D:\Games\NarutoRisePC"
```

To import an ISO into an existing empty `game/` folder, select **Install a new copy
or import an ISO**, or pass `/MODE=install /GAMEISO="D:\Original.iso"`.

### Tests

```powershell
python packaging/tests/test_setup_helper.py -v
$env:NARUTORISE_TEST_PACKAGE = 'C:\path\to\naruto-rise-recomp-win-amd64'
python packaging/tests/test_setup_inno.py -v
```

Integration tests compile with a unique test AppId and install into private
temporary directories with shortcuts and launcher execution disabled. Fixtures
are synthetic and contain no original game data. The test compiler path defaults
to `%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe`; override with
`NARUTORISE_TEST_ISCC`. Use `NARUTORISE_TEST_BUILD` for a custom helper build
directory. Tests compile two releases and exercise updates, legacy detection,
portable conversion, numeric version checks, and user data preservation.
See [installer verification](../docs/installer-status.md) for available checks and the manual validation checklist; record results for each release.

---

## 3. Launcher Features

The native C++ launcher (`narutorise_launcher.exe`):
- **Multi-language support:** English (default), Brazilian Portuguese, French, German, Spanish, Italian, and Russian, selectable in the header.
- **Game preferences:** Native game language selection (English, French, German, Spanish, Italian) and skipping intro videos in Settings → Game.
- **Graphic configuration:** Internal resolution scale (1x to 4x), Ultrawide (16:9, 21:9, 32:9), 60 Hz VSync, Anisotropic Filtering (up to 16x), Fullscreen.
- **Game file discovery:** Auto-detects `game/` or opens a native extracted-folder picker.
- **Clean launch:** Automatically persists choices to `narutorise.toml` and launches `narutorise.exe`.
