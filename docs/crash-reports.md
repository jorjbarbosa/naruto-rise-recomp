# Local diagnostics

Naruto Rise creates a separate session under `<effective data_root>/incidents/`
when opened directly, from a shortcut or from the AppImage. The first process
supervises the game process before SDL/runtime startup, drains stderr continuously
and preserves its exit status. It does not depend on the retired launcher.

Local Crashpad capture is enabled in the normal Sentry-enabled build even when
**Send crash reports** is off. Off sessions have no ingestion DSN and require SDK
consent. Upload authorization is separate from local capture. A fresh database is
used for every session; re-enabling uploads never reopens older databases.

The session includes a versioned manifest, bounded phase history, first renderer
failure, role progress, native exception dump when available, the session log
(last 2 MiB), and stderr (first 2 MiB). The parent keeps draining after that limit
so writing stderr cannot block the game. A fixed guest snapshot adds native PC,
fault address, guest/host thread relationship, memory base, LR/CTR and the published
GPR values. Compiler-local register values are not reconstructed; LR is not PC. The guest snapshot is an SDK exception observation: correlate its PC/thread with the original exception dump before treating it as the fatal context.
Normal sessions are identified separately from explicit subsystem failures and
interruptions of undetermined cause. A last log line does not identify the cause.

In **Settings > Advanced**, open the diagnostics folder, request a live thread
snapshot, or export a completed previous session to ZIP. A notification on the
next opening links to diagnostics from an interrupted session. Acknowledgment
does not delete the evidence. Manual snapshots also work when the UI is frozen:

```text
narutorise.exe --incident-request-snapshot "<session directory>"
narutorise.exe --incident-export "<completed session directory>" "<new ZIP path>"
```

These commands do not initialize SDL or the game. Linux uses the `narutorise`
executable with the same arguments. Export verifies hashes, excludes saves/game
files and rejects paths outside the session. Inspect the package before sharing:
dumps and logs can contain personal information and local paths.

Completed sessions are pruned on startup at 14 days, 32 sessions or 256 MiB.
Active captures are preserved even when above quota. Exported ZIPs under
`<data_root>/incident-exports/` are managed manually. Original log rotation remains
independent of the copies inside incidents. Deleting cache does not change upload
consent or delete incidents under the data root.

Windows live snapshots use MiniDumpWriteDump in a separate, time-limited worker.
Linux live snapshots use `/usr/bin/gdb` when available, with a 15 s timeout and a
128 MiB core limit; absence, permission failures and limits are recorded.
Exception capture uses Crashpad on both platforms. Missing helper, initialization
failure and missing dumps have explicit diagnostics. Fast-fail can bypass normal
handlers: WER sidecar presence or successful Sentry initialization does not prove
that coverage. The current Windows fast-fail fixture exited `0xC0000602` without a
WER dump; the manifest preserves that limitation and the observed status.

The supervisor records suspected inactivity after 120 s, using active work and
progress of UI, guest, translator, compiler and GPU separately. UI progress alone
does not clear a stuck worker. Collection does not terminate the game or declare
the cause. Loading, pause and minimized-window behavior must be considered when
interpreting a snapshot. Crashes before the executable entry point cannot be
captured retroactively; the supervising instance records a child that never
became ready. Power loss or loss of the supervisor may leave an incomplete
session; the next opening recovers it without inventing an exit status or crash.

## Developer verification

Use new isolated data/cache paths and empty game data. Never inherit credentials
into a disposable process whose memory is being captured. `--incident_test=`
supports `normal`, `abort`, `access-violation`, `worker-av`, `illegal`,
`observer-av`, `stack-overflow`, `fast-fail`, `stop`, `shutdown-blocked`, `hang`, `automatic-hang`,
`exit`, `revoke`, `revoke-pending` and `remote-failure`. The last one sends an event
only with explicit `crash_reporting=true`; all local fixtures default to opt-out.
`automatic-hang` runs for about 130 s to exercise the real inactivity threshold.

Tests: `incident_test.py` (supervisor/FailureChannel), `incident_export_test.py`
(archive/recovery/retention) and `incident_boot_test.py` (real app/dumps/opt-out).
Test success is distinct from reproducing the unknown fault on the machine in
issue 72. That machine still needs a build containing this collection system.
