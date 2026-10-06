# Configuration and ultrawide

[Documentation](README.md) · [DLCs](dlc.md)

## Where to configure settings

Use the launcher whenever possible. It reads and saves `narutorise.toml` **next to the executables**. Close both the game and launcher before editing the file manually.

During development, `app/narutorise.toml` is the template copied by CMake into the build output. Rebuilding can overwrite preferences saved there; make persistent development changes in the template or keep a backup of your configuration.

## Ultrawide

The launcher offers 16:9, 21:9, and 32:9. The patch changes the game's projection constant and passes the target aspect ratio to the SDK presenter, preserving the resolution seen by the original game code.

| Aspect ratio | `ultrawide_target_aspect` |
| --- | --- |
| Native 16:9 / disabled | `0.0` |
| Ultrawide, 3440×1440 profile | `2.3889` |
| Super ultrawide 32:9 | `3.5556` |

The profile labeled 21:9 corresponds to 3440 ÷ 1440. For another aspect ratio, use width ÷ height in the TOML file. The code accepts finite values between 0 and 8 and restores the native factor for ratios near or below 16:9.

```toml
ultrawide_target_aspect = 2.3889
fullscreen = true
```

The current template has **ultrawide enabled at `2.3889`**; select 16:9 for standard widescreen monitors. This is an anamorphic implementation: it does not imply individual HUD repositioning or fixes for every video and cutscene. The patch validates the original constant before changing it and logs any mismatch.

## Main settings

The values below match `app/narutorise.toml` at the time of the project analysis:

| Key | Value | Effect |
| --- | --- | --- |
| `launcher_language` | `"en"` | Launcher interface; use `"pt_BR"` for Brazilian Portuguese |
| `resolution_scale` | `2` | Internal scale; the launcher offers 1 through 4 |
| `draw_resolution_scale_x`, `draw_resolution_scale_y` | `2`, `2` | Draw scales, synchronized by the launcher |
| `fullscreen` | `true` | Fullscreen mode |
| `vsync`, `d3d12_host_vsync` | `true`, `true` | Vertical synchronization |
| `video_mode_refresh_rate` | `60.0` | Reported refresh rate; not a guarantee of 60 FPS |
| `swap_post_effect` | `"fxaa"` | Edge-smoothing post-processing |
| `present_effect` | `"cas"` | Presentation effect; integration depends on the SDK |
| `skip_intro_videos` | `true` | Skip intro videos |
| `show_fps_overlay` | `false` | FPS/frametime overlay |
| `input_backend` | `"sdl"` | SDL input |
| `mnk_mode` | `false` | Mouse and keyboard mode disabled by default |
| `log_level` | `"info"` | Logging level |

The launcher provides anisotropic filtering up to 16×. The internal `anisotropic_override` value uses the SDK's encoding; select the desired level through the interface.

`d3d12_readback_resolve = true` and `execute_unclipped_draw_vs_on_cpu = true` are part of the current graphics configuration. The latter is associated with the fix for black flickering during fights, together with the corresponding SDK patch.

## Shortcuts

| Key | Action |
| --- | --- |
| `F1` | Toggle the FPS and frametime overlay (shows the guest and host frame rates) |
| `F3` | Toggle the SDK debug overlay (guest frame stats, build stamp, and per-frame counters when built with perf counters) |
| `Alt+F4` | Request game shutdown |

The launcher language does not automatically change the languages available in the game.

The FPS overlay (`F1`) shows two rates: **Guest** — the game's own frame rate, measured on every guest swap, which is what game speed follows — and **Host** — the window present rate, which can be much higher on high-refresh displays. Slow motion with a high host rate means the guest rate is below the game's target.

## Folders and user data

| Content | Default Windows location |
| --- | --- |
| Game data in a distribution | `game/` next to the executable, or the folder selected in the launcher |
| Game data during development | `game_root/` at the checkout root |
| Configuration | `narutorise.toml` next to the executable |
| Logs | `logs/` next to the executable |
| Saves and user content | `narutorise/` inside the Windows Documents folder |
| Cache | `Documents/narutorise/cache/` |
| Installed DLCs | `Documents/narutorise/0000000000000000/555307E5/00000002/` |
| DLC packages awaiting import | `dlc/` next to the executable |

The Documents folder may be redirected to OneDrive. To find its actual location:

```powershell
Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'narutorise'
```

Back up the entire `narutorise/` folder in Documents to preserve saves, content, and metadata. The runtime supports overriding `user_data_root` and `cache_root`, but the launcher's DLC manager uses the default Documents path; custom locations require consistent manual organization.

To specify extracted game data and a Title Update:

```toml
custom_game_root = "D:/Games/Naruto/game"
update_data_root = "D:/Games/Naruto/title_update"
```

Use `/` in TOML strings or escape backslashes. See [DLCs and Title Updates](dlc.md) for more information.
