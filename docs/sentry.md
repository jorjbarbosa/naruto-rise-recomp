# Sentry Native

Organization: `naruto-rise`. Project: [`narutorise`](https://naruto-rise.sentry.io/settings/projects/narutorise/).
The Windows and Linux application share this project; the retired launcher is not instrumented.

Public ingestion DSN:

```text
https://a63d5e2aa01f37d95830c319efe87b5a@o4512325452496896.ingest.us.sentry.io/4512325773426688
```

The DSN identifies the destination for events. It is not an administration or symbol-upload credential.

## Player preference

Reporting is off by default, including for existing configurations without the new key.
Players can enable **Settings > Advanced > Send crash reports** and restart. The choice is
stored as `crash_reporting` in the active `narutorise.toml`, outside the cache. Local capture starts independently of uploads.

When enabled, the SDK sends native crash dumps and technical context, and samples 10% of
startup timing transactions. Crash dumps can contain personal information from process memory.
Session logs and the bounded diagnostics channel are attached only with consent. Saves and game files are not attached. Do not describe
dumps as guaranteed free of personal data. Disabling the setting revokes consent for new
uploads immediately; a request already in flight may have been received. Each session has its own database under `<data_root>/incidents/<session>/capture`. Off sessions use an empty DSN. Re-enabling requires a new session and never reopens older databases.

Local incident packages, fixed PowerPC context, explicit renderer failure events, retention, supervision and hang snapshots are described in [Local diagnostics](crash-reports.md).

## Build and package

`app/cmake/sentry.cmake` downloads the official 0.17.4 release archive with a pinned SHA-256.
With the game's Windows `clang++` driver, CMake builds the SDK separately using `clang-cl`
and stages `sentry.dll`; Crashpad's CMake requires the MSVC frontend for this toolchain.
Other supported toolchains, including Linux, link the SDK statically. Use the project's
Visual Studio 2022 x64 developer environment on Windows. Build options:

| CMake option | Default | Purpose |
| --- | --- | --- |
| `NARUTORISE_ENABLE_SENTRY` | `ON` | Include optional reporting; player consent is still required |
| `NARUTORISE_SENTRY_DSN` | DSN above | Select the ingestion destination |
| `NARUTORISE_SENTRY_ENVIRONMENT` | `production` | Default environment tag |

At runtime, nonempty `SENTRY_DSN`, `SENTRY_RELEASE` and `SENTRY_ENVIRONMENT` override the
build values. None bypasses the player's consent preference. The default release is
`narutorise@<version>+<git-revision>`; `-dirty` identifies a tracked worktree with local edits.
Native debug IDs distinguish individual binaries and their matching symbols.

Keep `sentry.dll` (when using the separate Windows SDK build), `crashpad_handler.exe`
and `crashpad_wer.dll` beside the executable on Windows, or
`crashpad_handler` with executable permission on Linux. `sentry-notices.txt` contains the
bundled redistribution notices. CMake stages these files; the Windows package script and
Linux AppImage script preserve them. An intermediate Linux package supplied to the AppImage
script must include the handler and notices from its Sentry-enabled build.

The app closes Sentry before the SDK's `_Exit` path, as well as during ordinary shutdown.
`patches/sdk/sentry-close-hook.patch` makes the existing `ReXApp::OnClosing` override
protected so the app can flush and delegate; this changes access, not the SDK's binary ABI.
Refresh the installed SDK header before building the app on either platform.
On Linux the SDK signal chain forwards unhandled fatal signals to the previous handler
while retaining recoverable runtime faults. The patch is recorded in
`patches/sdk/sentry-signal-chain.patch`; rebuild/install the SDK after applying it.

## Native symbols

Upload the **exact build being distributed**, including matching PDBs on Windows and ELF
debug information on Linux. The helper restricts selection to the application's binaries
and sidecars in the selected directory; it does not traverse other builds or upload source
files. Debug files can include original source paths and function names.

Install `sentry-cli` using [Sentry's instructions](https://docs.sentry.io/cli/installation/).
Set an organization auth token in `SENTRY_AUTH_TOKEN` in the local build environment or CI
secret store. Keep the token out of source files, command arguments and chat.

The Windows package script first archives matching application, SDK and guest symbols outside the public ZIP, under `out/release-symbols/`. It validates PE/PDB GUID and age and fails on missing symbols. For SDK source builds, retain Release line information (`-gline-tables-only`) and link shared libraries with `/DEBUG`; select another SDK symbol directory with `-SymbolRoot`.

Archive independently, then upload the exact archive before distribution:

```powershell
python scripts/archive-native-symbols.py app/out/build/win-amd64-release out/release-symbols/my-build --symbol-root sdk/out/win-amd64/Release
python scripts/upload-sentry-symbols.py out/release-symbols/my-build --dry-run
python scripts/upload-sentry-symbols.py out/release-symbols/my-build
```

The helper uses `sentry-cli debug-files upload --no-sources --wait` and fails if authentication, upload,
or the primary Windows PDB is missing. No sources are included. Preserve symbols separately
from the public player package. The app's entry point and reporting code have debug
information enabled; generated guest code retains its existing line-table setting.
Fully readable frames in SDK, guest modules and other dependencies require their exact
symbols too; the primary application PDB alone does not prove that coverage.

## Repeat verification for a new build

Use the rebuilt application's real initialization with new, isolated data and cache
directories. No game data is required for these developer modes, which stop before graphics
and guest execution. Set the environment to `sentry-setup` to distinguish these events from
player errors. Upload symbols first.

```powershell
$env:SENTRY_ENVIRONMENT = 'sentry-setup'
app/out/build/win-amd64-release/narutorise.exe --data_root="<new-isolated-data>" --cache_root="<new-isolated-cache>" --game_data_root="<empty-fixture-folder>" --crash_reporting=true --sentry_test=error
```

`--sentry_test=error` throws and catches `std::runtime_error` with the message
`Naruto Rise Sentry verification error`, captures its stack through the normal SDK instance,
flushes and exits. Without `--crash_reporting=true`, it exits with code 2 and does not send.
`--sentry_test=crash` intentionally aborts the disposable process to exercise Crashpad.

Use the Sentry plugin to find the corresponding issue and check its title, exception value,
environment, release and source file/line frames. Sending an event or observing an event ID
locally is not proof of receipt or symbolication. Also verify an ordinary setup/menu close
and the packaged handler in both supported operating systems before publishing a release.

The Linux signal-chain regression target is `narutorise_signal_chain_test`. Its six child
process cases exercise SIGSEGV/SIGILL recovery, forwarding the original signal context, and
fatal default behavior. Configuration regressions in `narutorise_config_manager_test` cover
default opt-out, persistence and revocation.
The Windows application driver is `app/tests/sentry_boot_test.py`; its optional `--crash`
argument adds the controlled abort. This driver sends real verification events to the
configured project and requires a new output directory for each invocation.

References: [Native SDK setup](https://docs.sentry.io/platforms/native/),
[Crashpad backend](https://docs.sentry.io/platforms/native/configuration/backends/crashpad/),
[tracing](https://docs.sentry.io/platforms/native/tracing/),
[native debug-file upload](https://docs.sentry.io/platforms/native/data-management/debug-files/upload/).
