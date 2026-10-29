# Configuration and ultrawide

[Documentation](README.md) · [DLCs](dlc.md)

## Where to configure settings

The initial menu opens by default. Turn off **Settings → Advanced → Show menu at startup** (`show_startup_menu = false`) to boot directly into the game. **F1** / **Back+Start** opens settings during play. Hold Shift while opening, or pass `--setup`, to request the initial menu again. Automated boots can use `--show_startup_menu=false`. Settings save automatically to `narutorise.toml` in the **active data folder**, shown in **Settings → Advanced**. The Windows ZIP defaults to `data/` beside the executable; Linux defaults to the XDG data directory. See [Data folder and recovery](data-folder.md). Close the game before editing the file manually. Options labelled RESTART require reopening the game; API/device changes offer a restart.

New control profiles use **Automatic** for player one: an available controller and keyboard/mouse work without manual assignment, including controllers connected after startup. Saved manual assignments and **None** remain explicit choices. Select **Automatic** in the controls screen to restore automatic selection for an existing profile.

Native builds configure controls through **Settings → Controls → Configure controls**, before playing or from the F1 menu. The dedicated four-port configuration is described below.

During play, resolution, brightness, frame rate, VSync, MSAA, FXAA, CAS/FSR, texture filtering, transparency AA, frame latency and triple buffering apply without restarting. The menu waits for the renderer to accept each change; a rejected change restores the previous value. Resolution, MSAA and presentation resource changes can briefly pause rendering. Vulkan keeps effective frame latency at 1 and respects the surface's supported image counts. Skip intro videos affects subsequent video opens.

Graphics API, graphics card, input API, game language/translation and game-file changes retain their restart behavior.

The shared Settings window has tabs and an option list, with descriptions or values in the adjacent panel:

| Group | Options |
| --- | --- |
| Game | Game language, skip intro videos |
| Display | Renderer (Direct3D 12 / Vulkan), graphics card, resolution, brightness, frame rate, VSync, fullscreen |
| Quality | MSAA, FXAA, post-processing (CAS/FSR), texture filtering, transparency AA |
| Controls | Dedicated four-port configuration, SDL/XInput backend, global menu shortcuts |
| Advanced | Frame latency, triple buffering, data folder, update checks, desktop shortcut |

Reset actions restore the recommended values for their page. The setup screen shows the installed version.

During development, CMake copies `app/narutorise.toml` to `defaults/narutorise.toml`. It initializes a missing active configuration once; rebuilding and replacing distribution files preserve the active TOML. Edit the template for factory defaults, or the active file for personal preferences.

**Renderer** selects **Direct3D 12** or **Vulkan**, in that order, from the APIs compiled into the game. There is no separate Xenos/native selector. The menu saves `renderer = "native"` and `native_backend = "d3d12"` or `"vulkan"`; the choice takes effect on the next game launch. Old Xenos selections migrate to the native renderer while preserving a saved native backend. With no backend key, a build containing D3D12 uses it; a Vulkan-only build uses Vulkan. An explicitly requested API that is not compiled in produces an error.

Settings exclusive to Xenos are hidden in native menus. Builds without the native renderer retain their legacy settings and do not offer the native API selector.

Graphics cards are listed by the selected API, using their names rather than sharing adapter indices between DXGI and Vulkan. Changing the API resets the card selection to automatic. Vulkan supports the same render scale, MSAA, gamma/brightness, CAS/FSR and FXAA paths. Its current presentation adapter limits the queue to one pending presentation even when a larger frame latency is configured. VSync uses FIFO; disabling it chooses immediate presentation when tearing is allowed and available, otherwise mailbox or FIFO.

## Game language

Open **Settings → Game → Game language** in the menu. The selection is saved with the other settings and applied on the next game launch. Text and voice availability depends on the language assets included in your game files.

| Language | `user_language` |
| --- | --- |
| English (default) | `1` |
| French | `4` |
| German | `3` |
| Spanish | `5` |
| Italian | `6` |

For example, set `user_language = 5` in `narutorise.toml` to select Spanish. This preference is independent of `launcher_language`. The menu preserves manually configured SDK language codes outside this list, displaying them as **Custom (code)** until you select a listed language.

External [translation mods](translation-mods.md) live in `<active data folder>/mods/` and appear in this selector by their package names. Selecting one saves `translation_mod = "folder-name"` and `user_language = 1` in the active TOML; selecting an official language clears that selection. Reopen the menu after installing a package. Changes take effect after restart. The selection and mods follow a data-folder change after verified copying. Missing or invalid packages are omitted from the list and the runtime retains original English text; the saved selection remains until another language is chosen. The distributed PT-BR seed in `defaults/mods/pt-br` initializes an absent active package and preserves existing customizations during boot, rebuild and update. See [data-folder recovery](data-folder.md).

## Menu and compatibility bridge languages

Both interfaces offer languages in this order:

| Interface language | `launcher_language` |
| --- | --- |
| English (default) | `"en"` |
| Brazilian Portuguese | `"pt_BR"` |
| French | `"fr"` |
| German | `"de"` |
| Spanish | `"es"` |
| Italian | `"it"` |
| Russian | `"ru"` |

Use **Settings → Game → Menu language** to change the interface. This does not change the game language: the game selector offers the five native languages listed above plus installed translation packages.

The installer always starts with English selected, independently of the Windows language or a previous installation's language. Numbered installer options preserve the order above. On a fresh installation, the selected installer language becomes the menu language. Reinstallation preserves existing configuration, including language preferences.

## Ultrawide

Xenos supports 16:9, 21:9, and 32:9 through the TOML settings below. The native menu hides these Xenos-only options. The patch changes the game's projection constant and passes the target aspect ratio to the SDK presenter, preserving the resolution seen by the original game code.

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

The template and **Reset to Recommended** use **16:9 (`0.0`)**; select 21:9 or 32:9 for ultrawide monitors. This is an anamorphic implementation: it does not imply individual HUD repositioning or fixes for every video and cutscene. The patch validates the original constant before changing it and logs any mismatch.

## Main settings

The values below match `app/narutorise.toml`:

| Key | Value | Effect |
| --- | --- | --- |
| `launcher_language` | `"en"` | Menu interface; see the seven language codes above |
| `user_language` | `1` | Game language; see the language codes above |
| `translation_mod` | `""` | External translation package under active data `mods/`; requires English and a restart |
| `resolution_scale` | `1` | Internal scale, the SDK's (and Xenia's) default; the menu offers 1× through 4× |
| `draw_resolution_scale_x`, `draw_resolution_scale_y` | `1`, `1` | Draw scales, synchronized by the menu |
| `graphics_device` | `""` | Graphics card by the name Windows gives it (both renderers); empty is automatic. See [Graphics card](#graphics-card) |
| `fullscreen` | `true` | Fullscreen mode |
| `vsync`, `d3d12_host_vsync` | `true`, `true` | Vertical synchronization |
| `video_mode_refresh_rate` | `60.0` | Reported refresh rate; not a guarantee of 60 FPS |
| `swap_post_effect` | `"fxaa"` | Edge-smoothing post-processing |
| `present_effect` | `"cas"` | Presentation effect; integration depends on the SDK |
| `skip_intro_videos` | `true` | Skip boot logos (`Ubisoft.bik`, `LOGO_CORPO_EMEA.bik`, `LOGO_CORPO_US.bik`); preserve the campaign opening (`Intro.bik`) |
| `show_fps_overlay` | `false` | FPS/frametime overlay |
| `input_backend` | `"sdl"` | Physical input provider (`sdl` or `xinput`); changing it requires a game restart |
| `mnk_mode` | `false` | Legacy keyboard/mouse selection; native Automatic and per-port profiles take precedence |
| `log_level` | `"info"` | Logging level |

The menu provides anisotropic filtering up to 16×. The internal `anisotropic_override` value uses the SDK's encoding; select the desired level through the interface.

`d3d12_readback_resolve = true` and `execute_unclipped_draw_vs_on_cpu = true` are part of the current graphics configuration. The latter is associated with the fix for black flickering during fights, together with the corresponding SDK patch.

## Renderer

Builds with the native renderer (`NARUTORISE_NATIVE_RENDERER_BOOTSTRAP=ON`, see [Build and run](build-and-run.md)) offer these choices under **Settings → Display → Renderer**, according to the APIs compiled in:

| Renderer | `native_backend` |
| --- | --- |
| Direct3D 12 (default when compiled) | `"d3d12"` |
| Vulkan | `"vulkan"` |

[Renderers and frame rate](renderers.md) explains how the two differ, how the native renderer presents 60 and 120 FPS, and its known issues.

The menu saves `renderer = "native"` for both choices. Xenos remains available as an advanced runtime configuration (`renderer = "xenos"`) when launching the game directly; opening a native menu migrates that choice to native. Builds without the native renderer use Xenos and retain their legacy options. The following table describes runtime support; Xenos-only controls are hidden in native menus.

| Option | Xenos | Native |
| --- | --- | --- |
| Resolution (`resolution_scale`) | 1× to 4× | 1×, 1.5× (`internal_resolution_scale = 1.5`), 2× to 4× |
| Texture filtering, FXAA, CAS/FSR, VSync, fullscreen | Yes | Yes |
| Aspect ratio (`ultrawide_target_aspect`) | Yes | No: the image stays 16:9 with bars |
| Dithering (`present_dither`), FPS overlay (`show_fps_overlay`, F1), F3 overlay | Yes | No |
| MSAA, transparency antialiasing, frame rate, brightness, frame latency, triple buffering | No | Yes |

With the native renderer, VSync does not change the game speed: the game paces itself.

Native renderer options:

| Key | Default | Effect |
| --- | --- | --- |
| `msaa_samples` | `0` | MSAA of the game's multisampled targets: `0` keeps the game's 4×; `2`, `4` or `8` |
| `transparency_antialiasing` | `false` | Alpha to coverage instead of the alpha test on foliage, fences and other cut-out textures |
| `frame_rate` | `30` (`60` in the shipped file and the menu) | Frame rate: `30` keeps the game's; `31` to `240` keep the game's logic at its own step and interpolate the frames in between (the menu's slider: 30, 60, 120). Where the step is not well below the frame, the frames follow the step instead, so the game keeps its speed: fights (step 1/60 s) show at 60 below `76`, the open world (2/60 s) at 30 from `31` to `37`. With VSync on, it does not go above the monitor's refresh rate |
| `brightness` | `0.5` | Output brightness; `0.5` is the game's |
| `internal_resolution_scale` | `0.0` | Fractional internal scale from 1 to 4 (`1.5` = 1920×1080); `0` uses `resolution_scale` |
| `max_frame_latency` | `1` | Frames queued ahead of the display (1 to 3) |
| `triple_buffering` | `false` | Three swapchain buffers instead of two |

## Graphics card

The menu enumerates graphics cards through the selected API (**Settings → Display → Graphics card**) and saves the chosen name in `graphics_device`. Vulkan and DXGI indices are never shared:

- **Automatic** (`graphics_device = ""`): the native renderer takes the card with the most dedicated video memory; Xenos takes the first card Windows lists.
- **A named card:** the native renderer asks Plume for it; Xenos sets the SDK's `d3d12_adapter` to its index for that launch only. Vulkan rejects a missing or ambiguous name. The D3D12 path falls back to automatic with a warning when the named card is unavailable.

The executable also exports `NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance`, so hybrid laptops (NVIDIA Optimus, AMD PowerXpress) run the game on the dedicated card. The log names the card in use: `DXGI adapter:` for Xenos, `Native renderer adapter:` for the native renderer.

## Keyboard and mouse

Choose **Keyboard and mouse** for the desired player under **Configure controls**. Each port stores independent controller and keyboard/mouse mappings. One device belongs to one player; transferring an occupied device requires confirmation and leaves its previous port without a device, preserving both ports' adjustments. These ports do not add multiplayer modes to the game.

Select a binding to capture input **in that row**, with the list still visible. Release the input that started capture, then press the new input. **Esc** cancels; **Del** removes only the selected binding. Navigation, player/category changes and menu shortcuts are suspended during capture and until the captured input is released; B/LB/RB remain bindable inputs. Other alternatives stay in place. Keyboard conflicts are resolved only within that port's keyboard bank, including equivalent saved key aliases.

Buttons, D-pad/stick clicks, triggers, movement and camera have keyboard/mouse views. The mouse turns J1's exploration camera directly, respecting remapped/removed directions; elsewhere it supplies the mapped guest axes. A keyboard assigned to another port supplies that port's axes without turning J1's direct camera. Mouse sensitivity ranges from **1% to 1000%** (`0.01`–`10.0`), as supported by the SDK. Alt+Tab releases pointer capture. F1, F3, F4, F7, F11, Backtick and the current global menu key remain reserved. Shift, Control and Alt can be standalone buttons; remove such a standalone binding before using the same modifier in a chord.

| Input | Default keys |
| --- | --- |
| Left stick | W, A, S, D |
| Right stick (camera) | Mouse, arrow keys |
| A | Space |
| B | E, Backspace |
| X | Left mouse button, J |
| Y | Right mouse button, K |
| LB / RB | Q / R |
| LT / RT | Ctrl / Shift |
| L3 / R3 | C / middle mouse button, V |
| Start / Back | Enter, Esc / Tab |
| D-pad | 1, 2, 3, 4 (up, down, left, right); numpad 8, 2, 4, 6 |

The native menu saves schema `controls.version = 1`, with `[controls.player1]` through `[controls.player4]`, and `.pad`/`.keys` banks below each port. Keys use comma-separated SDK names, including `LMB`, `RMB`, `MMB`, `X1MB`, `X2MB` and mapped mouse directions (`MouseUp`, `MouseDown`, `MouseLeft`, `MouseRight`). Existing alternatives are retained. Migration imports `mnk_mouse`, `mnk_sensitivity` and `keybind_*` into J1 and selects **Automatic** (`device = "auto"`), which follows an available controller and also accepts keyboard/mouse. Devices explicitly assigned to another port are excluded from automatic selection. Existing versioned manual profiles remain unchanged. Global legacy fields remain in the file, but per-port profiles become authoritative. `mouse_camera_direct = false` retains the stick-based camera path everywhere; `mouse_camera_trace = true` logs direct camera motion.

SDL combines the real serial and device path when both are available (otherwise it uses the available identifier); a session-only identity requires reassignment after restarting. Identical names have distinct suffixes. Device/driver path changes can require reassignment. XInput assignments follow its native slots, without a persistent physical identity. Ambiguous identities require reassignment. Device availability and ownership update on connection/disconnection; stale transfers and captures are canceled. **None** reports the port disconnected rather than presenting the SDK's NOP stand-in as hardware.

Deadzones ignore small unwanted input near rest. The selected port has separate left/right stick and trigger values (0–95%) and an application toggle. Live plots show mapped device readings before filtering, the ignored region and effective output. Keyboard/mouse and unsupported axes do not offer analog adjustments. Supported devices offer per-channel vibration intensity, a brief **Identify** pulse and a stoppable test capped at five seconds. Leaving, changing player/category/device, losing focus, closing or disconnecting cancels the test. A removed backend may no longer accept a final motor command; physical shutdown behavior requires hardware verification.

Confirmed changes save automatically and apply live after a successful write. Canceled captures/transfers/restores do not save. Invalid files or failed writes keep the last effective state. **Restore defaults** asks for confirmation, resets both banks plus deadzones/sensitivity/vibration for that port, and keeps its assigned device, other players and global menu shortcuts.

## Shortcuts

| Key | Action |
| --- | --- |
| `F1` or `Back+Start` | Open the native in-game menu (remappable in Controls) |
| Shift while opening, or `--setup` | Open the setup screen |
| `Alt+Enter` or `F11` | Toggle between borderless fullscreen and windowed mode (both renderers) |
| `F3` | Toggle the SDK debug overlay (guest frame stats, build stamp, and per-frame counters when built with perf counters; Xenos renderer) |
| `Alt+F4` | Request game shutdown |

**Show FPS** in Settings → Advanced shows the swap rate and frame time. F1 opens the native menu. The SDK F3/F4 overlays are disabled on the native renderer. `REX_NATIVE_FPS_TITLE=1` additionally enables frame statistics in the window title for diagnostics.

## Folders and user data

| Content | Default Windows ZIP location |
| --- | --- |
| Game data | External ISO or complete extracted copy selected in setup |
| Factory template | `<exe>/defaults/narutorise.toml` |
| Active configuration | `<exe>/data/narutorise.toml` |
| Logs | `<exe>/data/logs/` |
| Saves, profiles and achievements | Xbox trees under `<exe>/data/user/` |
| Cache | `<exe>/data/cache/` |
| Installed DLCs | `<exe>/data/user/0000000000000000/555307E5/00000002/` and adjacent `Headers/` |
| DLC packages awaiting import | `<exe>/data/dlc/` |

Linux defaults to `${XDG_DATA_HOME:-$HOME/.local/share}/narutorise`, with cache
in `${XDG_CACHE_HOME:-$HOME/.cache}/narutorise`. The explicit portable mode uses
`data/` beside the external AppImage, including its cache. The data folder menu
shows active and pending locations separately and applies changes after restart.

`--data_root` > `REX_DATA_ROOT` > persisted selection > platform default. General
session overrides isolate storage without implicit legacy import. The existing
`--user_data_root` > `REX_USER_DATA_ROOT` > TOML contract still selects the exact
guest tree, with no `saves/` suffix. CLI/env relative storage roots use the initial
cwd; relative TOML paths retain the historical executable/import base. Explicit
cache/log paths prevail. With an explicit guest root and no explicit cache root,
cache follows that guest root; otherwise it follows the platform policy above.

Back up the complete active folder, any custom guest root, and the selection
file. Windows legacy Documents is discovered through the system API, including
redirected/OneDrive folders. First-run import copies eligible data and preserves
the source. [Data folder and recovery](data-folder.md) describes conflicts,
interrupted copies, portable selection and backups.

To specify extracted game data and a Title Update:

```toml
custom_game_root = "D:/Games/Naruto/game"
update_data_root = "D:/Games/Naruto/title_update"
```

`custom_game_root` can also name the Xbox 360 disc image itself, which the game reads in place without extracting it. Full disc images and trimmed XISO images both work:

```toml
custom_game_root = "D:/Games/Naruto - Rise of a Ninja.iso"
```

With an image, a Title Update is looked for in `title_update/` or `update/` next to the image. On startup the log names the game data and whether its `default.xex` is the one this port was recompiled from (`Game data: ...: ready`).

Use `/` in TOML strings or escape backslashes. See [DLCs and Title Updates](dlc.md) for more information.
