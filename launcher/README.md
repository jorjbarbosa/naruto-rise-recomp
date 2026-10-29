# Retired launcher sources

The supported executable is `narutorise.exe`, with embedded RmlUi setup and
in-game menus. CMake no longer configures this directory or builds the old
ImGui launcher, installer, ISO helper or updater UI.

Shared configuration, content, localization, key names, graphics enumeration
and update-checking code moved to `app/src/menu`. Their host regression tests
are in `app/tests`. The application icon is `app/ui/narutorise.ico`.

Remaining sources/assets are historical material, outside the build and release
payload. See [release packaging](../packaging/README.md) for the ZIP and the
temporary Inno compatibility bridge.
