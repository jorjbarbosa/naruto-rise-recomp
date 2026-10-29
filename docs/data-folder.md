# Data folder and recovery

The active folder is shown under **Settings → Advanced**. It contains one active
`narutorise.toml`, `logs/`, active mods in `mods/`, pending DLC packages in `dlc/`, and the guest
root in `user/`: the Xbox content and profile trees (saves, installed DLC, title data and
achievements). A folder from an earlier version, with those trees at its root, has them moved
into `user/` on the next start; a tree present in both places stops the start with a message
instead of choosing one. An explicit `user_data_root` names the guest root itself.
Game ISOs/extracted data and native modules stay separate.

| Distribution/mode | Data folder | Default cache |
| --- | --- | --- |
| Windows ZIP | `data/` beside `narutorise.exe` | `data/cache/` |
| Linux AppImage | `${XDG_DATA_HOME:-$HOME/.local/share}/narutorise/` | `${XDG_CACHE_HOME:-$HOME/.cache}/narutorise/` |
| Linux portable, explicitly selected | `data/` beside the external AppImage | `data/cache/` |

Empty or relative XDG variables fall back to the home-based locations. The
AppImage mount and original cwd never choose the default writable data folder.
`defaults/narutorise.toml` in a package is an immutable factory template: a new
configuration is initialized from it only when no active/eligible legacy TOML exists.
Rebuilds and ZIP/AppImage replacement preserve active preferences.

All active mods, including translations, are loaded from `<active data folder>/mods`.
Local diagnostic sessions live in `<active data folder>/incidents` and manually
exported ZIPs in `incident-exports`; neither follows a cache override. See
[crash reports and export](crash-reports.md) for retention and collection limits.
When changing the data location, previous incident folders remain at the original
root; export them there. New sessions use the newly selected root.
The menu and game use the same resolved path. Mods follow a folder change only
after restart and verified copying; an explicit guest/save root does not move mods.

The complete PT-BR package is distributed as an immutable seed in
`defaults/mods/pt-br`. Preparation imports eligible old mods first, then copies
this seed only if the active `mods/pt-br` package is absent. An existing package
is preserved in full, including partial or customized packages. Initialization,
rebuilds, ZIP/AppImage replacement and the compatibility bridge never refresh
active translation files automatically. To adopt a new translation version,
close the game, back up your active package and deliberately merge/replace it
with the new seed. Removing the whole active `pt-br` folder restores the seed
on the next boot after eligible legacy import has completed. The translation
selection stays in the active TOML; copying a package alone does not select it.

## Change the location

Choose **Open data folder**, **Choose another folder**, **Use portable folder**
or **Restore default folder** in Advanced. A writable, separate target is required;
a parent/child of the active folder or a folder held by another writer is refused.
The active and pending folders are shown separately. Changes take effect after
restart; until the process closes, all settings and game writes keep using the
active folder. The next boot copies and verifies the final source state, keeps
the source, and activates the selection only after success.

The bootstrap selection lives in `data_location.json`, outside the movable
preference contract. A distribution descriptor takes precedence over the user
one (`LocalAppData/narutorise/` on Windows, `${XDG_CONFIG_HOME:-$HOME/.config}/narutorise/`
on Linux). Explicit portable selection writes the distribution descriptor.
Restoring/changing a portable preference updates that descriptor so it cannot
hide the new choice. Merely creating a Linux `data/` folder does not select it.

## Session and guest overrides

`--data_root` takes precedence over `REX_DATA_ROOT`, persisted selection and the
platform default. It selects only that session and skips implicit personal-data
import. The menu can open the folder but blocks persistent selection while this
override is active. Restart without the override to edit the saved location.
This also skips legacy mods beside the executable/AppImage and in old AppRun
caches. A new isolated folder may receive the curated PT-BR seed from the package,
but no personal mod is imported implicitly.

`--user_data_root` > `REX_USER_DATA_ROOT` > TOML still chooses the exact guest
content root; custom guest storage is not relocated implicitly. Advanced lists an explicit save/content folder separately so it can be opened and backed up. Explicit cache
and `log_file` paths retain priority. With an explicit guest root but no explicit
cache, cache follows `<guest>/cache`. CLI/environment relative storage paths use
the initial cwd; imported relative TOML paths retain their historical base.
Prefer absolute custom guest/cache paths for AppImage installations. ISO/game
CLI overrides remain transient; selecting a game in the menu saves that choice
in the active TOML. Restart and Windows shortcuts preserve explicit overrides
without inventing a guest override from a derived default.

## Legacy import and backups

Windows discovers the real system Documents folder, including redirection, and
imports eligible `narutorise` saves/content into `user/`. On Linux the original XDG
guest tree is the data folder itself, so its trees move into `user/` in place.
Old executable-side TOML/logs/DLC and the previous AppRun
XDG configuration/logs/staging are copied as appropriate; old logs are namespaced
under `logs/legacy/`. All profiles, headers, licenses and achievements retain
their relative trees and bytes. Existing active TOML takes precedence, including
reporting a parse error instead of replacing an invalid file with defaults.
A custom `dlc_source_path` is retained; empty disables new package scanning.
Mods beside the old executable, beside the external AppImage, and in completed
AppRun payload caches are copied to active `mods/` through the same recoverable
journal. This import also runs once for data folders initialized before mods
were included. Equal files are accepted; divergent contents block preparation
and preserve both copies. Resolve differences after backing up, then retry.
The runtime never falls back to loading mods from the old installation.

An interrupted AppImage initialization can resume from a different mount when
its immutable defaults/seed match the recorded inventory. If those resources
changed, retry with the original package; the stage and active data are kept.

Close the game, then back up the entire active folder, any custom guest root,
and the selection file. Preserve `.narutorise-migration/`: its journal and source
completion records support interrupted imports and prevent stale reimport. Keep
original copies until loading and saving your existing game data has been checked.
The remaining `.narutorise.lock` file is normal; close other instances rather than
deleting a live lock. The OS releases the lock when its process exits.

If preparation fails, the guest does not start with a partial tree. The error
shows the paths/recovery condition; early diagnostics go to an explicit log file
or a temporary `narutorise/bootstrap/` log, plus stderr/system message. Preserve
both source and destination. Resolve differing files after backing them up and
retry with the original selection. A malformed TOML/descriptor/journal must be
repaired or restored from backup; it is never silently reset. An isolated session
can use a different `--data_root` without changing the saved selection.

AppImage updates replace the image file only. Old payload caches are not removed
automatically. Before manually clearing them, confirm no explicit guest/config
path still uses that location; imported relative paths may intentionally retain
an old cache base. Back up custom guest storage separately. Data/default cache
separation cannot protect files deliberately stored inside a cache being deleted.
