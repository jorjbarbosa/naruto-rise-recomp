# Release packaging

The primary distribution is `naruto-rise-recomp-win-amd64.zip`. Extract it to a
writable folder and open `narutorise.exe`. On first launch, select your original
Xbox 360 ISO or `default.xex` from a complete extracted copy. ISOs are read
directly. Hold Shift while opening, or pass `--setup`, to return to setup.
Press F1 or Back+Start during play for Settings, DLC and Exit game.

See [Build and run](../docs/build-and-run.md), [Configuration](../docs/configuration.md)
and [DLCs](../docs/dlc.md).

## Build the ZIP

Run in the Visual Studio x64 development environment with CMake, Ninja, Clang
and `llvm-readobj.exe` available.

The Release build must enable `NARUTORISE_NATIVE_RENDERER_BOOTSTRAP=ON` so the
ZIP has its first-launch setup screen. Packaging rejects a cache without it
before building or staging files; the [build guide](../docs/build-and-run.md)
includes the required option:

```powershell
pwsh -File packaging/package-release.ps1
# Use a separate output folder or override the default release version:
pwsh -File packaging/package-release.ps1 -OutputRootDir dist/candidate -Version 0.3.0-beta
```

The script refreshes the Release build and generated DLC modules, audits PE
imports for debug CRT dependencies, and stages the complete package before
replacing any previous output. `-Build` remains accepted for compatibility.
The default version comes from `packaging/version.iss`; the staged
`narutorise.version` records the selected version.

Outputs are `naruto-rise-recomp-win-amd64/` and its ZIP. The payload includes:

- `narutorise.exe`, runtime/plugin DLLs and every generated character DLC engine;
- optional `amd_fidelityfx_dx12.dll` when provided by the SDK;
- default `narutorise.toml`, the version file and a quick-start README;
- curated `mods/pt-br/mod.toml` and `texts.json`;
- Xenos shader storage from `NARUTORISE_SHADER_STORAGE_DIR`;
- project and third-party notices under `licenses/`, including RmlUi,
  FreeType (FTL) and the embedded Roboto Condensed font (OFL).

Menus, artwork and fonts are embedded in the executable. The ZIP contains no
launcher, setup helper, `extract-xiso`, `install.ps1` or installer. Game data,
personal settings, saves and locally installed mods are never copied into it.
The icon source is `app/ui/narutorise.ico`. The extractor remains a developer
tool for preparing the code-generation inputs, outside the distribution.

The Xenos shader storage must be available in `packaging/private/shader_cache/`
or the configured CMake cache directory. This private, ignored folder contains
game-derived data. Native shader packages are already embedded in the game;
DXC, validation layers and Vulkan SDK tools are not distributed.

## Compatibility bridge

For several releases, also attach `NarutoRiseInstaller.exe` so the old 0.3.0
launcher updater can reach the new distribution. Older launchers consult only
the latest published release; removing this asset too soon strands that path.
The final release carrying the bridge has not been selected.

Build it explicitly **after** the ZIP, with Inno Setup 6 installed:

```powershell
pwsh -File packaging/package-bridge.ps1
# Or build it from a candidate payload:
pwsh -File packaging/package-bridge.ps1 -OutputRootDir dist/candidate
```

This command uses the staged payload and its recorded version. It produces
only the extra bridge asset and does not compile the game. Failed compilation
leaves the previous asset intact. `-PackageDir` and `-IsccPath` can select an
alternate payload/compiler.

The bridge retains the established AppId, asset name and `/MODE=update` handoff.
It detects registered Inno and older native installations, including custom
destinations. A manually selected portable copy is also accepted when
`narutorise.exe` and `rexruntime.dll` exist. It replaces the port binaries and
bundled translation, redirects the known Start menu/desktop shortcuts to the
game, and removes `narutorise_launcher.exe` after copying the new files.
Existing desktop/legacy portable links are migrated only when they target
this installation. Links to other copies in those two locations are preserved;
the standard Start menu link follows the installation selected in the bridge.

There is no ISO page, extraction helper or ISO import. The bridge preserves
the current installation when the launcher is in use: preflight reserves that
file before replacing any payload, and reports an error if it cannot do so.
Close the old launcher and retry. The bridge also preserves
`game/`, `game_root/`, existing `narutorise.toml` byte for byte, saves, DLC,
community mods and unrelated files, including on uninstall. It does not run
legacy uninstallers. It keeps downgrade guards and the historical version
numbering migration. Fresh installation remains available as a compatibility
convenience; new users should use the ZIP.

The old updater passes `/MODE=update`, `/DIR`, `/LANG`, `/NORESTART` and
`/NOFORCECLOSEAPPLICATIONS`. Manual automation can also use `/MODE=install`;
invalid modes and missing update destinations fail before file replacement.
The completion page opens `narutorise.exe`. For a manual ZIP update, close the
game and replace package files while retaining your existing TOML and data.
An old portable launcher is left untouched by ZIP extraction and may be deleted
manually. The new game only announces updates and opens the release page.

## Verification

Host regressions now live in `app/tests`, with their implementation in
`app/src/menu`; no launcher directory is configured by CMake:

```powershell
cmake --build app/out/build/win-amd64-release --target narutorise_config_manager_test narutorise_key_bindings_test narutorise_localization_test narutorise_game_languages_test narutorise_update_checker_test
# Supply unique scratch directories to tests that write fixtures.
app/out/build/win-amd64-release/narutorise_config_manager_test.exe out/config-test
app/out/build/win-amd64-release/narutorise_key_bindings_test.exe
app/out/build/win-amd64-release/narutorise_localization_test.exe
app/out/build/win-amd64-release/narutorise_game_languages_test.exe out/languages-test .
app/out/build/win-amd64-release/narutorise_update_checker_test.exe
```

Verify the actual ZIP and run real Inno installation/migration/uninstall tests:

```powershell
python -I packaging/tests/test_package.py dist/naruto-rise-recomp-win-amd64.zip
$env:NARUTORISE_TEST_PACKAGE = "$PWD/dist/naruto-rise-recomp-win-amd64"
$env:NARUTORISE_TEST_OLD_PACKAGE = "$PWD/dist/v0.3.0-beta/naruto-rise-recomp-win-amd64"
$env:NARUTORISE_TEST_OUTPUT = "$PWD/out/bridge-tests"
python -I packaging/tests/test_bridge.py -v
```

Bridge tests use a unique AppId, private shortcut folders and synthetic saves.
They do not start the launcher or game. With an old payload supplied, they
compile the pinned original installer with isolated locations and exercise
its installed-file/uninstall history before updating through `/MODE=update`.
Its unused ISO-helper slot is a fixture; ISO extraction is not exercised.
Logs remain in the output folder. Without that payload, the registered 0.3.0
case is explicitly skipped. Portable, legacy registration, downgrade,
language and shortcut-ownership cases still run.

Native setup, overlay and user-root boots are opt-in and require the user's ISO:

```powershell
python -I app/tests/setup_boot_test.py dist/naruto-rise-recomp-win-amd64 C:/path/game.iso out/setup-boots
python -I app/tests/overlay_boot_test.py dist/naruto-rise-recomp-win-amd64 C:/path/game.iso out/overlay-boots
python -I app/tests/user_data_boot_test.py dist/naruto-rise-recomp-win-amd64 C:/path/game.iso out/user-root-boots
```

Use a new output folder for each run. These tests create isolated products/user
roots and display game windows. Proton/Game Mode, physical controllers and
updater download/UI handoff still require separate acceptance testing.
