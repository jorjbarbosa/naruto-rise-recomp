# Installer verification

[Documentation](README.md) · [Packaging](../packaging/README.md)

This document lists available tests and the verification workflow. The existence of tests does not mean they have passed on the current build; record results for each release.

## Existing tests

| File | Scope |
| --- | --- |
| `packaging/tests/test_game_paths.cpp` | Launcher game data discovery |
| `packaging/tests/test_setup_helper.py` | XEX/Title ID validation, synthetic ISOs, and extraction helper |
| `packaging/tests/test_setup_inno.py` | Installer and uninstaller integration in temporary directories |

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
- Run the portable package and select an extracted game folder.
- Switch launcher languages, graphics settings, and 16:9/ultrawide aspect ratios.
- Import DLC and launch a build containing `narutorise_ai2c2.dll`.
- Check saves, logs, and cache in the documented locations.
- Uninstall and confirm user data preservation.

Record the version, environment, command/procedure, result, and log. Real ISO compatibility and the installer's visual behavior require manual verification in addition to automated fixtures.
