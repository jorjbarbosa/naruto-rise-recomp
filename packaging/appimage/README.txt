Naruto Rise Recompiled — Linux AppImage

Make the AppImage executable and open it. Select your original ISO or a complete
game folder. If no game is selected, game/ and game_root/ beside the AppImage
are also checked. The ISO stays in its original location.

Data: ${XDG_DATA_HOME:-$HOME/.local/share}/narutorise/
Active configuration: narutorise.toml in that folder.
Logs: logs/; DLC packages: dlc/; saves/profiles/installed DLC: the Xbox tree.
Rebuildable cache: ${XDG_CACHE_HOME:-$HOME/.cache}/narutorise/
The executable and resources are read directly from the image, without copying
the payload to the cache. defaults/narutorise.toml is the factory template.

Settings > Advanced shows the active folder and lets you open it, choose another,
use data/ beside the AppImage (portable), or restore the default. Changes take
effect after restarting; the game keeps writing to the active folder until it
closes. Data is copied and the source is preserved. --data_root or REX_DATA_ROOT
selects a folder for the session and prevents persistent changes through the menu.
A user_data_root override selects the exact guest root, without a saves/ suffix.

On first launch, eligible legacy data is imported by copying. The previous
wrapper's XDG configuration, logs and staging in old caches remain at the source.
Keep originals and .narutorise-migration until you have verified your backups.
Conflicts block startup: preserve both copies, resolve differing files and retry.
An explicit data_root override does not automatically import personal data.

To update, close the game and replace the AppImage file. Keep the data folder
and, in portable mode, data_location.json. Back up the entire active folder,
any saves/content in a custom user_data_root, and the location selection.
Before clearing old caches, confirm they do not contain a custom guest root
still in use; legacy relative paths retain their historical base.

Without FUSE, use: ./NarutoRise.AppImage --appimage-extract-and-run
Audio wait test: ./NarutoRise.AppImage --test-audio-wait
The test must finish with PASS; check music, effects and transitions in the game.
The AppImage does not include commercial game files.
