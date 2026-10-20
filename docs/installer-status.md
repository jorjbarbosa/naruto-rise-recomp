# Installer verification

[Documentation](README.md) · [Packaging](../packaging/README.md)

This document lists available tests and the verification workflow. The existence of tests does not mean they have passed on the current build; record results for each release.

## Existing tests

| File | Scope |
| --- | --- |
| `packaging/tests/test_game_paths.cpp` | Launcher game data discovery |
| `packaging/tests/test_setup_helper.py` | XEX/Title ID validation, synthetic ISOs, and extraction helper |
| `packaging/tests/test_setup_inno.py` | Installation, updates between releases, legacy/portable migration, version checks, and uninstall preservation in temporary directories |
| `packaging/tests/test_dlc_runtime.py` | Opt-in startup test of the base game and each character DLC engine; needs user-supplied game files and content ([DLCs](dlc.md#6-startup-regression-test)) |
| `launcher/tests/test_config_manager.cpp` | Independent game/interface language persistence and configuration compatibility |
| `launcher/tests/test_localization.cpp` | Seven complete translations, language order, codes, and formatting placeholders |

After building the required binaries, run from the repository root:

```powershell
cmake --build app/out/build/win-amd64-release --target narutorise_game_paths_test narutorise_setup_helper
.\app\out\build\win-amd64-release\narutorise_game_paths_test.exe
python .\packaging\tests\test_setup_helper.py -v
```

For integration tests, prepare a distribution and make Inno Setup available:

```powershell
$env:NARUTORISE_TEST_PACKAGE = (Resolve-Path .\dist\naruto-rise-recomp-win-amd64).Path
python .\packaging\tests\test_setup_inno.py -v
```

See the test sources for requirements and compiler paths. ISO fixtures are synthetic; integration tests use a dedicated AppId and temporary installations.

## Manual verification for each release

- Install under a standard user account and launch through the shortcut.
- Import a compatible ISO, including a path with spaces or accented characters.
- Cancel an import and verify that no partial `game/` directory was published.
- Reinstall while preserving existing data and preferences.
- Update an Inno, legacy native, and portable installation through **Update an existing installation**, without importing an ISO again.
- Confirm the folder picker selects the existing folder directly, the ISO page is skipped, the ready summary shows versions, and completion reports an update.
- Reject updates to a folder missing the port executables and reject older numeric release versions without changing existing files.
- Run the portable package and select an extracted game folder.
- Verify the installer starts in English and offers English, Portuguese, French, German, Spanish, Italian, and Russian in that order.
- Switch all seven launcher languages, including Cyrillic text, graphics settings, and 16:9/ultrawide aspect ratios.
- Confirm the bundled **Português (Brasil)** translation appears alongside the five native game languages on new installations and updates, without changing the saved selection. Interface language remains independent.
- Update an older bundled translation and check that other mod packages and personal files remain intact.
- Import DLC and launch a build containing `narutorise_ai2c2.dll`.
- For builds with the native renderer: launch once with each renderer, check that the options of the other renderer are greyed out, and play with the frame rate at 30, 60, and 120.
- Check saves, logs, and cache in the documented locations.
- Uninstall and confirm user data preservation.

Record the version, environment, command/procedure, result, and log. Real ISO compatibility and the installer's visual behavior require manual verification in addition to automated fixtures.

## Update verification — 2026-10-08

Inno Setup 6.7.3 compiled the manual update installer. All 17 integration tests
passed on Windows using the existing helper build at `out/language-check/build`
and the synthetic package at `out/language-check/installer-fixture`:

```powershell
$env:NARUTORISE_TEST_BUILD = (Resolve-Path .\out\language-check\build).Path
$env:NARUTORISE_TEST_PACKAGE = (Resolve-Path .\out\language-check\installer-fixture).Path
python .\packaging\tests\test_setup_inno.py -v
```

The suite compiled versions 1.0.0 and 1.0.1 under one unique test AppId, verified
binary replacement and the updated registration, and checked byte-for-byte
preservation of configuration, original game files, pending/installed DLCs,
saves, logs and personal files through update and uninstall. Legacy custom-path
detection, unknown-version portable migration, repeated updates, explicit ISO
import in install mode, and rejection of invalid destinations/modes, update ISO
imports, and older numeric releases passed. The game was never launched and
these synthetic packages are not distributable game builds. Interactive wizard
layout and real game launch remain manual checks.

The local distributable `dist/NarutoRiseInstaller.exe` was assembled as version
1.0.1 (17,506,758 bytes). Its game/runtime binaries came from the existing 1.0.0
package in `C:\Users\Jorge\dev\naruto-rise-recomp\dist\naruto-rise-recomp-win-amd64`;
the launcher/helper Release build, configuration, assets and installer came from
this checkout. PE import checks rejected debug CRT dependencies. The assembly
recipe and source binary hashes are recorded locally in
`out/manual-update-check/assemble-release.ps1` and `release-provenance.json`.
The installer SHA-256 is
`B180DBFA063ECFEA55464780DD0D571955168B3F7B8C88D2EC7143550257DF65`.

All 17 integration tests also passed against that assembled real package in
119.987 seconds, with `NARUTORISE_TEST_PACKAGE` pointing to
`dist/naruto-rise-recomp-win-amd64`. This run used unique GUID AppIds with the
same brace escaping as production. Install/update/uninstall were exercised in
temporary destinations; the production registration and game were not touched.

## Beta 1 migration verification — 2026-10-11

Issue #11 reports beta 1's internal `1.0.0-beta.1` version being treated as
newer than `0.1.0-beta.2`. Before the fix, the integration test using that
exact previous version failed with installer exit code 7. The beta 2 migration
allowlist now includes `1.0.0-beta.1` alongside the existing old labels.

All 17 Inno integration tests passed in 66.245 seconds using
`out/language-check/installer-fixture` and the helper from
`app/out/build/win-amd64-release`. The tests use unique test AppIds and temporary
destinations. Coverage includes beta 1 metadata read from the version file,
Inno registration and legacy registration; preservation of configuration,
game data, DLCs, saves and personal files; and rejection of the reverse
transition and unrelated newer versions (`1.0.0-beta.2`, `1.0.0-beta.10`,
`1.0.10`). The game was not launched.

The three update, version-check and downgrade tests also passed against the
actual beta 2 package in `dist/v0.1.0-beta.2-update/naruto-rise-recomp-win-amd64`
in 104.793 seconds, with isolated test AppIds. That directory's original
installer hash matches the GitHub beta 2 release asset. A corrected installer
was compiled locally from the same package into
`dist/issue-11-fix/NarutoRiseInstaller.exe` (20,325,174 bytes), with SHA-256
`065690add96792cc27ab2826d60a4468d3d4f7a29f22160c685352d23eabf11a`.
It has not been published; interactive wizard validation remains a manual check.

## Beta 3 artifact preparation — 2026-10-11

Local release files are in `dist/v0.1.0-beta.3/`. This is an installer and
metadata update using the verified published beta 2 portable payload from
`dist/v0.1.0-beta.2-update/`. The game, launcher, runtime and all three character
DLC engine DLLs retain their beta 2 hashes. Unrelated local native-renderer
experiments are excluded; no game/runtime rebuild was performed.

The installer product version and portable marker both read `0.1.0-beta.3`.
The numbering-reset exception covers beta 3 as well as beta 2, allowing beta 1's
`1.0.0-beta.1` identifier and the old local labels to migrate. The integration
suite also covers beta 2 metadata found only in the Inno registration.

The assembly recipe is `out/release-v0.1.0-beta.3/assemble.py`. Binary import
checks found no debug CRT dependencies. Every file in the ZIP matches its
portable source, and `game/` contains only the distribution placeholder.
No GitHub release or tag was created. Gameplay and the interactive wizard
remain outside this verification scope.

All 18 installer integration tests passed against the beta 3 portable package
in 196.908 seconds, using unique test AppIds and temporary destinations.
The beta 1 migration and beta 2 registry-only update both preserved user files.
The test log, release notes, source patch, build provenance and SHA-256 checksums
are stored alongside the local installer and ZIP.
