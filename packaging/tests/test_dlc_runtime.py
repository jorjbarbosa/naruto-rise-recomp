"""Opt-in startup regression test using user-supplied game and DLC files.

python packaging/tests/test_dlc_runtime.py --package <build-or-package> \
    --game <extracted-game> --content <user-data-root> --output <new-test-folder>

Copies binaries and marketplace content into an isolated test directory. It
never changes installed content, saves, or settings. Logs remain in --output.
This verifies engine loading and process survival, not gameplay correctness.
"""

import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import shutil
import subprocess
import time


PACKS = {
    "shikamaru": "AEFD8711E05CF01634F0053A5AB2E3D2520E1F0A55",
    "jiraiya_sarutobi": "41281D418DC64EA2F1818C1F9579A50764A2912A55",
    "choji_temari": "8B52C12F7F1DE81CFCCA5FD9895A76A08942960F55",
    "voices": "C78F6DCC1CE07BAB765DD845BB01FBC6D863440E55",
}
CASES = [
    ("base", [], "ai2c.dll"),
    ("shikamaru", ["shikamaru"], "ai2c@1.dll"),
    ("jiraiya_sarutobi", ["voices", "jiraiya_sarutobi"], "ai2c@2.dll"),
    ("choji_temari", ["choji_temari"], "ai2c@3.dll"),
    ("all", list(PACKS), "ai2c@3.dll"),
]


def close_game(process):
    """Request normal window shutdown, then stop only our child if necessary."""
    if process.poll() is not None:
        return
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]

    @callback_type
    def request_close(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == process.pid:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
        return True

    user32.EnumWindows(request_close, 0)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=10)


def run(args):
    for source in (args.package, args.game, args.content):
        if not source.is_dir():
            raise ValueError(f"Missing input directory: {source}")
    title = Path("0000000000000000/555307E5")
    for pack in PACKS.values():
        if not (args.content / title / "00000002" / pack).is_dir():
            raise ValueError(f"Missing test DLC: {pack}")
    args.output.mkdir(parents=True, exist_ok=False)
    runtime = args.output / "runtime"
    runtime.mkdir()
    shutil.copy2(args.package / "narutorise.exe", runtime)
    for dll in args.package.glob("*.dll"):
        shutil.copy2(dll, runtime)
    if (args.package / "shader_cache").is_dir():
        shutil.copytree(args.package / "shader_cache", runtime / "shader_cache")
    (runtime / "narutorise.toml").write_text(
        'gpu_plugin = "xenos"\nfullscreen = false\nvsync = true\n'
        'skip_intro_videos = true\ndlc_source_path = ""\nlog_level = "debug"\n'
        'ultrawide_target_aspect = 0.0\nresolution_scale = 1\n'
        'draw_resolution_scale_x = 1\ndraw_resolution_scale_y = 1\n'
        'd3d12_readback_resolve = true\nexecute_unclipped_draw_vs_on_cpu = true\n',
        encoding="utf-8",
    )
    results = []
    for name, packs, engine in CASES:
        if args.case and name not in args.case:
            continue
        user = args.output / name / "user"
        user.mkdir(parents=True)
        (user / title / "00000002").mkdir(parents=True)
        for pack_name in packs:
            pack = PACKS[pack_name]
            relative = title / "00000002" / pack
            shutil.copytree(args.content / relative, user / relative)
            header = title / "Headers/00000002" / (pack + ".header")
            (user / header).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.content / header, user / header)
        log = args.output / name / "runtime.log"
        command = [str(runtime / "narutorise.exe"), f"--game_data_root={args.game}",
                   f"--user_data_root={user}", f"--log_file={log}"]
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0  # SW_HIDE: no new console/helper window
        process = subprocess.Popen(command, cwd=runtime, startupinfo=startup,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        print(f"Running {name}: expected {engine}", flush=True)
        try:
            deadline = time.monotonic() + args.seconds
            while process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.25)
            survived = process.poll() is None
            early_exit = process.poll()
        finally:
            close_game(process)
        text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
        registered = f"Module '{engine}' registered " in text
        content_matches = f"ContentManager lists {len(packs)} marketplace package(s)" in text
        fatal = any(marker in text for marker in (
            "[critical]", "function not in function table", "layout does not match loaded XEX"))
        result = {"case": name, "engine": engine, "survived_seconds": args.seconds if survived else None,
                  "early_exit": early_exit, "registered": registered,
                  "expected_content": content_matches, "fatal": fatal,
                  "passed": survived and registered and content_matches and not fatal,
                  "log": str(log)}
        results.append(result)
        print(json.dumps(result), flush=True)
        (args.output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    return 0 if all(result["passed"] for result in results) else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("package", "game", "content", "output"):
        parser.add_argument(f"--{name}", type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--case", choices=[name for name, _, _ in CASES], action="append",
                        help="Run only selected scenarios (repeatable; default: all)")
    options = parser.parse_args()
    if options.seconds < 10:
        parser.error("--seconds must be at least 10")
    raise SystemExit(run(options))
