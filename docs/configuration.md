# Configuration and ultrawide

[Documentation](README.md) · [DLCs](dlc.md)

## Where to configure settings

Use the launcher whenever possible. It reads and saves `narutorise.toml` **next to the executables** when you play or close it. Close both the game and launcher before editing the file manually.

The launcher's **Settings** tab lists its groups on the left; the panel below the options describes the option under the mouse:

| Group | Options |
| --- | --- |
| Game | Game language, skip intro videos |
| Display | Renderer (Direct3D 12 / Vulkan), graphics card, resolution, brightness, frame rate, VSync, fullscreen |
| Quality | MSAA, FXAA, post-processing (CAS/FSR), texture filtering, transparency AA |
| Controls | SDL controllers, the connected controller, keyboard and mouse, mouse camera and sensitivity, key mapping |
| Advanced | Frame latency, triple buffering |

**Reset to Recommended** restores the recommended values; the launcher shows the game's version under the cover art.

During development, `app/narutorise.toml` is the template copied by CMake into the build output. Rebuilding can overwrite preferences saved there; make persistent development changes in the template or keep a backup of your configuration.

**Renderer** selects **Direct3D 12** or **Vulkan**, in that order, from the APIs compiled into the game. There is no separate Xenos/native selector. The launcher saves `renderer = "native"` and `native_backend = "d3d12"` or `"vulkan"`; the choice takes effect on the next game launch. Old Xenos selections migrate to the native renderer while preserving a saved native backend. With no backend key, a build containing D3D12 uses it; a Vulkan-only build uses Vulkan. An explicitly requested API that is not compiled in produces an error.

Settings exclusive to Xenos are hidden in native-capable launchers. Builds without the native renderer retain their legacy settings and do not offer the native API selector.

Graphics cards are listed by the selected API, using their names rather than sharing adapter indices between DXGI and Vulkan. Changing the API resets the card selection to automatic. Vulkan supports the same render scale, MSAA, gamma/brightness, CAS/FSR and FXAA paths. Its current presentation adapter limits the queue to one pending presentation even when a larger frame latency is configured. VSync uses FIFO; disabling it chooses immediate presentation when tearing is allowed and available, otherwise mailbox or FIFO.

## Game language

Open **Settings → Game → Game language** in the launcher. The selection is saved with the other settings and applied on the next game launch. Text and voice availability depends on the language assets included in your game files.

| Language | `user_language` |
| --- | --- |
| English (default) | `1` |
| French | `4` |
| German | `3` |
| Spanish | `5` |
| Italian | `6` |

For example, set `user_language = 5` in `narutorise.toml` to select Spanish. This preference is independent of `launcher_language`. The launcher preserves manually configured SDK language codes outside this list, displaying them as **Custom (code)** until you select a listed language.

External [translation mods](translation-mods.md) installed in `mods/` next to the executable appear in this same selector, using only their package names. Selecting one saves `translation_mod = "folder-name"` and `user_language = 1` automatically; selecting an official language clears the mod selection. Installation alone does not activate a package. The list refreshes when the launcher opens, regains focus, or starts the game. If the selected package is missing or invalid, it disappears from the list and English is selected automatically, without a warning. Restoring a package does not reactivate it. Changes take effect on the next game launch.

## Launcher and installer languages

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

Use the selector in the launcher's header to change its interface. This does not change the game language: the game selector offers the five native languages listed above plus installed translation packages.

The installer always starts with English selected, independently of the Windows language or a previous installation's language. Numbered installer options preserve the order above. On a fresh installation, the selected installer language becomes the launcher language. Reinstallation preserves existing configuration, including language preferences.

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

The template and **Reset to Recommended** use **16:9 (`0.0`)**; select 21:9 or 32:9 for ultrawide monitors. This is an anamorphic implementation: it does not imply individual HUD repositioning or fixes for every video and cutscene. The patch validates the original constant before changing it and logs any mismatch.

## Main settings

The values below match `app/narutorise.toml`:

| Key | Value | Effect |
| --- | --- | --- |
| `launcher_language` | `"en"` | Launcher interface; see the seven language codes above |
| `user_language` | `1` | Game language; see the language codes above |
| `translation_mod` | `""` | External translation package under `mods/`; requires English and a restart |
| `resolution_scale` | `1` | Internal scale, the SDK's (and Xenia's) default; the launcher offers 1× through 4× |
| `draw_resolution_scale_x`, `draw_resolution_scale_y` | `1`, `1` | Draw scales, synchronized by the launcher |
| `graphics_device` | `""` | Graphics card by the name Windows gives it (both renderers); empty is automatic. See [Graphics card](#graphics-card) |
| `fullscreen` | `true` | Fullscreen mode |
| `vsync`, `d3d12_host_vsync` | `true`, `true` | Vertical synchronization |
| `video_mode_refresh_rate` | `60.0` | Reported refresh rate; not a guarantee of 60 FPS |
| `swap_post_effect` | `"fxaa"` | Edge-smoothing post-processing |
| `present_effect` | `"cas"` | Presentation effect; integration depends on the SDK |
| `skip_intro_videos` | `true` | Skip boot logos (`Ubisoft.bik`, `LOGO_CORPO_EMEA.bik`, `LOGO_CORPO_US.bik`); preserve the campaign opening (`Intro.bik`) |
| `show_fps_overlay` | `false` | FPS/frametime overlay |
| `input_backend` | `"sdl"` | SDL input |
| `mnk_mode` | `false` | Keyboard and mouse as a controller; see [Keyboard and mouse](#keyboard-and-mouse) |
| `log_level` | `"info"` | Logging level |

The launcher provides anisotropic filtering up to 16×. The internal `anisotropic_override` value uses the SDK's encoding; select the desired level through the interface.

`d3d12_readback_resolve = true` and `execute_unclipped_draw_vs_on_cpu = true` are part of the current graphics configuration. The latter is associated with the fix for black flickering during fights, together with the corresponding SDK patch.

## Renderer

Builds with the native renderer (`NARUTORISE_NATIVE_RENDERER_BOOTSTRAP=ON`, see [Build and run](build-and-run.md)) offer these choices under **Settings → Display → Renderer**, according to the APIs compiled in:

| Renderer | `native_backend` |
| --- | --- |
| Direct3D 12 (default when compiled) | `"d3d12"` |
| Vulkan | `"vulkan"` |

[Renderers and frame rate](renderers.md) explains how the two differ, how the native renderer presents 60 and 120 FPS, and its known issues.

The launcher saves `renderer = "native"` for both choices. Xenos remains available as an advanced runtime configuration (`renderer = "xenos"`) when launching the game directly; opening a native-capable launcher migrates that choice to native. Builds without the native renderer use Xenos and retain their legacy options. The following table describes runtime support; Xenos-only controls are hidden in native-capable launchers.

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
| `frame_rate` | `30` (`60` in the shipped file and the launcher) | Frame rate: `30` keeps the game's; `31` to `240` keep the game's logic at its own step and interpolate the frames in between (the launcher's slider: 30, 60, 120). Where the step is not well below the frame, the frames follow the step instead, so the game keeps its speed: fights (step 1/60 s) show at 60 below `76`, the open world (2/60 s) at 30 from `31` to `37`. With VSync on, it does not go above the monitor's refresh rate |
| `brightness` | `0.5` | Output brightness; `0.5` is the game's |
| `internal_resolution_scale` | `0.0` | Fractional internal scale from 1 to 4 (`1.5` = 1920×1080); `0` uses `resolution_scale` |
| `max_frame_latency` | `1` | Frames queued ahead of the display (1 to 3) |
| `triple_buffering` | `false` | Three swapchain buffers instead of two |

## Graphics card

The launcher enumerates graphics cards through the selected API (**Settings → Display → Graphics card**) and saves the chosen name in `graphics_device`. Vulkan and DXGI indices are never shared:

- **Automatic** (`graphics_device = ""`): the native renderer takes the card with the most dedicated video memory; Xenos takes the first card Windows lists.
- **A named card:** the native renderer asks Plume for it; Xenos sets the SDK's `d3d12_adapter` to its index for that launch only. Vulkan rejects a missing or ambiguous name. The D3D12 path falls back to automatic with a warning when the named card is unavailable.

The executable also exports `NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance`, so hybrid laptops (NVIDIA Optimus, AMD PowerXpress) run the game on the dedicated card. The log names the card in use: `DXGI adapter:` for Xenos, `Native renderer adapter:` for the native renderer.

## Keyboard and mouse

Turn on **Settings → Controls → Use keyboard and mouse**. The keyboard and mouse act as an Xbox controller and work alongside a connected controller. The game still shows the controller's buttons.

- **Mouse moves the camera.** While exploring, the mouse turns the camera itself, at a fixed angle per pixel: at 100% **Sensitivity** (from 10% to 300%), one full turn takes about 2,100 pixels. The camera turns at most about 340 degrees per second, the speed the game's camera keeps up with; faster motion finishes over the next frames. Elsewhere, such as in fights, the mouse stands in for the right stick. While the game window has focus, the cursor stays inside it; Alt+Tab releases it.
- **Map keys…** gives each controller input two keys. Click a field and press the new key within 5 seconds; right-click a field to clear it. A key already in use leaves its previous input. F1, F3, F4, F7 and Backtick belong to the game's overlays and cannot be mapped. **Restore default keys** brings back the layout below.

Keys are recorded by their position on the keyboard, as the game reads them, and shown with your layout's labels: on an ABNT2 keyboard, the key to the right of L shows as Ç.

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

In `narutorise.toml`, the options are `mnk_mode`, `mnk_mouse`, `mnk_sensitivity` (`1.0` = 100%) and one `keybind_*` per input, such as `keybind_a = "Space"`. A value lists alternatives separated by commas, with the SDK's key names (`LMB`, `RMB` and `MMB` for the mouse buttons). Shift, Control and Alt work as keys of their own. A combination such as `Shift+Up` works only while that modifier is not bound on its own. `mouse_camera_direct = false` makes the mouse stand in for the right stick everywhere, and `mouse_camera_trace = true` writes each mouse turn of the camera to the log.

With keyboard and mouse on, closing the game takes about 3 seconds, as with a controller connected.

## Shortcuts

| Key | Action |
| --- | --- |
| `F1` | Toggle the FPS and frametime overlay (shows the guest and host frame rates; Xenos renderer) |
| `F3` | Toggle the SDK debug overlay (guest frame stats, build stamp, and per-frame counters when built with perf counters; Xenos renderer) |
| `Alt+F4` | Request game shutdown |

The FPS overlay (`F1`) shows two rates: **Guest** — the game's own frame rate, measured on every guest swap, which is what game speed follows — and **Host** — the window present rate, which can be much higher on high-refresh displays. Slow motion with a high host rate means the guest rate is below the game's target.

The window title shows the game's name and version. The native renderer has no overlays; with the environment variable `REX_NATIVE_FPS_TITLE=1` (set for your Windows user, so the launcher passes it on), it adds the frame rate and frame time to the window title, visible in windowed mode.

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

The game reads `user_data_root` before `narutorise.toml`, so in the TOML file it takes effect only when the launcher starts the game: the launcher passes it on the command line (a relative path is relative to the launcher's folder). Started directly, the game takes it from `--user_data_root` or the `REX_USER_DATA_ROOT` environment variable.

To specify extracted game data and a Title Update:

```toml
custom_game_root = "D:/Games/Naruto/game"
update_data_root = "D:/Games/Naruto/title_update"
```

Use `/` in TOML strings or escape backslashes. See [DLCs and Title Updates](dlc.md) for more information.
