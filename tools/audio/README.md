# Audio battery

Loopback recordings of the game's music, and the analysis of repeated blocks and
dropouts that reproduced issue #86. Every change of the audio path goes through
it before a test build goes out.

Acceptance of the native audio layer: in each variant, at most 1% of repeated
chunks and no dropout, on Windows and on a real Linux machine. WSLg does not
count: its rendering is software and its sound goes through RDP.

## Windows

```
python -m venv venv
venv\Scripts\pip install numpy soundcard
venv\Scripts\python.exe -I tools\audio\record_menu.py <product dir> <game root> <output dir> [--layer native|sdk] wasapi dsound dsound-2400
```

- `<product dir>` holds `narutorise.exe`: a build folder or an unpacked package.
- Each variant runs with its own data folder in `%TEMP%`.
- It records 12 s of the title and 30 s of the main menu, and writes the
  analysis and the audio lines of the log to `summary.json`.
- Variants:
  - `wasapi`: the default device;
  - `dsound`: `SDL_AUDIO_DRIVER=directsound`;
  - `dsound-2400`: DirectSound plus `SDL_AUDIO_DEVICE_SAMPLE_FRAMES=2400`, the 50 ms
    blocks of the #86 report. On WASAPI SDL keeps the period near 10 ms, so the
    variable only matters on DirectSound.

## Linux

```
tools/audio/battery-linux.sh NarutoRise-<version>-x86_64.AppImage <output dir> [default sdl-2400 quantum-2400]
```

- It runs the AppImage with `--audio_layer=native` in each variant and records
  30 s of the title.
- Then it waits for the tester to open the main menu, and records 30 s of it.
- Variants:
  - `default`;
  - `sdl-2400`: `SDL_AUDIO_DEVICE_SAMPLE_FRAMES=2400`;
  - `quantum-2400`: `PIPEWIRE_QUANTUM=2400/48000`.
- It needs `parecord` or `pw-record`, and `python3` with numpy.

## Analysis

```
python -I tools/audio/analyze.py <recording.npy|recording.wav>...
```

- **Repeats:** the share of 10 ms chunks that repeat a chunk from 5 to 200 ms
  earlier (correlation above 0.97), with the dominant lags.
- **Dropouts:** windows of 2 ms at least 30 dB under the level around them.

Reference, main menu music on Windows, 30 s, as repeats / dropouts:

| Build | wasapi | dsound | dsound-2400 |
|---|---|---|---|
| 0.4.0-beta | 0.8% / 0 | 0.8% / 0 | 48.7% / 600 |
| 0.4.0-beta with the SDK's frame pacing (PR #88) | 0.8% / 0 | 0.8% / 0 | 0.6% / 0 |

The #86 reporter's video showed 12% repeats, dominant lag exactly 2400 samples.

## Recording the title's frames

`REX_NATIVE_AUDIO_DUMP=<file.wav>` records every frame the game delivers, before
the mix, in both layers:
- 6 channels, 32-bit float, 48 kHz;
- channel order FL FR C LFE SL SR.

Recording the same route once per layer compares the two paths.
