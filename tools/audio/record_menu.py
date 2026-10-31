"""The audio battery on Windows: boots a build to the main menu and records its music by loopback.

Usage: python -I record_menu.py <product dir> <game root> <output dir> [--layer native|sdk] <variant>...
Variants: wasapi | dsound (SDL_AUDIO_DRIVER=directsound)
          | dsound-2400 (+ SDL_AUDIO_DEVICE_SAMPLE_FRAMES=2400, the block of issue #86).

Each variant boots the product in place with its own data folder under %TEMP%
(native renderer, Vulkan, keyboard mode, no startup menu, the given
audio_layer), records 12 s of the title and 30 s of the main menu from the
default output device, sends Enter (START) between them and closes with
Alt+F4. The recordings go through analyze.py; summary.json keeps the results
and the audio lines of the log. The game takes the screen and plays audio:
nothing else may play meanwhile.

Python: python -m venv venv; venv\\Scripts\\pip install numpy soundcard
"""
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

import numpy as np
import soundcard as sc

sys.path.insert(0, str(Path(__file__).parent))
import analyze  # noqa: E402

args = sys.argv[1:]
layer = "native"
if "--layer" in args:
    at = args.index("--layer")
    layer = args[at + 1]
    del args[at:at + 2]
source, game, output = (Path(a).resolve() for a in args[:3])
variants = args[3:]
repo = Path(__file__).resolve().parents[2]
tests = repo / "app" / "tests"
spec = importlib.util.spec_from_file_location("close_test", tests / "close_boot_test.py")
close = importlib.util.module_from_spec(spec)
sys.path.insert(0, str(tests))
spec.loader.exec_module(close)
win = close.win

ENVS = {
    "wasapi": {},
    "dsound": {"SDL_AUDIO_DRIVER": "directsound"},
    "dsound-2400": {"SDL_AUDIO_DRIVER": "directsound", "SDL_AUDIO_DEVICE_SAMPLE_FRAMES": "2400"},
}


def press(hwnd, vk):
    close.focus_window(hwnd)
    events = (close.Input * 2)()
    for i, up in enumerate((False, True)):
        events[i].type = 1
        events[i].key = close.Keyboard(vk, win.user32.MapVirtualKeyW(vk, 0), 2 if up else 0, 0, 0)
    assert win.user32.SendInput(1, ctypes.byref(events[0]), ctypes.sizeof(close.Input)) == 1
    time.sleep(0.12)
    assert win.user32.SendInput(1, ctypes.byref(events[1]), ctypes.sizeof(close.Input)) == 1


def record(seconds, path):
    speaker = sc.default_speaker()
    mic = sc.get_microphone(id=str(speaker.name), include_loopback=True)
    with mic.recorder(samplerate=48000, channels=2) as rec:
        data = rec.record(numframes=int(48000 * seconds))
    np.save(path, data.astype(np.float32))
    return speaker.name


def stop_product(product):
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    f"Get-Process narutorise -ErrorAction SilentlyContinue | "
                    f"Where-Object {{ $_.Path -like '{product}*' }} | Stop-Process -Force"], check=False)


output.mkdir(parents=True, exist_ok=True)
summary = []
for variant in variants:
    data = Path(tempfile.mkdtemp(prefix="narutorise-audio-"))
    config = dict(renderer="native", native_backend="vulkan", fullscreen=False, skip_intro_videos=True,
                  frame_rate=120, mnk_mode=True, custom_game_root=str(game), check_for_updates=False,
                  show_startup_menu=False, crash_reporting=False, crash_reporting_asked=True, audio_layer=layer)
    (data / "data").mkdir()
    (data / "data/narutorise.toml").write_text(
        "\n".join(f"{k} = {json.dumps(v)}" for k, v in config.items()), encoding="utf-8")
    env = dict(os.environ, REX_DATA_ROOT=str(data / "data"), REX_USER_DATA_ROOT=str(data / "user"))
    env.update(ENVS[variant])
    process = subprocess.Popen([str(source / "narutorise.exe")], cwd=source, env=env)

    def log():
        files = list((data / "data/logs").glob("*.log"))
        return max(files, key=lambda p: p.stat().st_mtime).read_text(encoding="utf-8", errors="replace") if files else ""

    name = f"{layer}-{variant}"
    result = dict(variant=variant, layer=layer, data=str(data))
    try:
        hwnd = win.wait_for(lambda: win.window(process.pid), process, timeout=60)
        win.wait_for(lambda: "Native renderer backend:" in log(), process, timeout=60)
        time.sleep(10)
        result["device"] = record(12, output / f"{name}-title.npy")
        try:
            press(hwnd, 0x0D)
            result["start_sent"] = True
        except AssertionError as error:
            result["start_sent"] = str(error)
        time.sleep(5)
        win.capture(hwnd, output / f"{name}-menu.png")
        record(30, output / f"{name}-menu.npy")
        try:
            close.alt_f4(hwnd)
            process.wait(timeout=20)
            result["exit_code"] = process.returncode
        except Exception as error:  # noqa: BLE001
            result["close"] = str(error)
    finally:
        time.sleep(1)
        stop_product(source)
        if process.poll() is None:
            process.kill()
        text = log()
        (output / f"{name}.log").write_text(text, encoding="utf-8")
        result["audio_log"] = [line.split("] ", 3)[-1] for line in text.splitlines()
                               if "[narutorise] Audio" in line or "audio endpoint" in line
                               or "AudioSystem::" in line][:40]
    menu = output / f"{name}-menu.npy"
    if menu.exists():
        result["analysis"] = analyze.analyze(str(menu))
    summary.append(result)
    (output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "audio_log"}), flush=True)
    shutil.rmtree(data, ignore_errors=True)
