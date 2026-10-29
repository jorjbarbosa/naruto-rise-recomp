"""Install a loose DLC fixture through the native folder picker and shared page.

Usage: python -I app/tests/dlc_boot_test.py <product folder> <game.iso> <new output folder>
All configuration/content is isolated in Temp. No personal DLC is read or changed.
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

spec = importlib.util.spec_from_file_location('setup_test', Path(__file__).with_name('setup_boot_test.py'))
win = importlib.util.module_from_spec(spec)
spec.loader.exec_module(win)
focus_spec = importlib.util.spec_from_file_location('close_test', Path(__file__).with_name('close_boot_test.py'))
focus = importlib.util.module_from_spec(focus_spec)
focus_spec.loader.exec_module(focus)


def main():
    source, iso, output = (Path(arg).resolve() for arg in sys.argv[1:])
    output.mkdir(parents=True, exist_ok=False)
    product = Path(tempfile.mkdtemp(prefix='narutorise-dlc-'))
    for item in source.iterdir():
        if item.name == 'narutorise.exe' or item.suffix.lower() == '.dll': shutil.copy2(item, product/item.name)
    config = dict(renderer='native', native_backend='d3d12', fullscreen=False, custom_game_root=str(iso),
                  launcher_language='pt_BR', check_for_updates=False)
    (product / 'data').mkdir(exist_ok=True)
    (product/'data/narutorise.toml').write_text('\n'.join(f'{key} = {json.dumps(value)}' for key,value in config.items()), encoding='utf-8')
    content = product/'sample-dlc'
    content.mkdir(); (content/'content.bin').write_bytes(b'dlc integration test')
    helper = output/'select-folder.ps1'
    helper.write_text('''param([int]$AppProcessId, [string]$Folder)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName System.Windows.Forms
$condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $AppProcessId)
$until = [DateTime]::UtcNow.AddSeconds(5)
$field = $null
while (!$field -and [DateTime]::UtcNow -lt $until) {
 $windows = [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $condition)
 $elements = $windows | ForEach-Object { $_.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition) }
 $field = $elements | Where-Object { $_.Current.AutomationId -eq '1152' -and $_.Current.ControlType -eq [System.Windows.Automation.ControlType]::Edit } | Select-Object -First 1
 if (!$field) { Start-Sleep -Milliseconds 100 }
}
if ($field) {
 $field.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($Folder)
} else {
 # Some Windows builds omit the folder-name edit. Navigate the address bar.
 $search = $elements | Where-Object { $_.Current.AutomationId -eq 'SearchEditBox' } | Select-Object -First 1
 if (!$search) { throw 'Native folder picker was not found' }
 $search.SetFocus()
 $shell = New-Object -ComObject WScript.Shell
 [void]$shell.AppActivate($AppProcessId)
 $address = $null
 foreach ($shortcut in @('^l','%d')) {
  [System.Windows.Forms.SendKeys]::SendWait($shortcut)
  $addressUntil = [DateTime]::UtcNow.AddSeconds(2)
  while (!$address -and [DateTime]::UtcNow -lt $addressUntil) {
   Start-Sleep -Milliseconds 100
   $elements = $windows | ForEach-Object { $_.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition) }
   $address = $elements | Where-Object { $_.Current.AutomationId -eq '1001' -and $_.Current.ControlType -eq [System.Windows.Automation.ControlType]::Edit } | Select-Object -First 1
  }
  if ($address) { break }
 }
 if (!$address) { throw 'Native address edit was not found' }
 $address.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($Folder)
 $address.SetFocus()
 [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
 Start-Sleep -Milliseconds 300
 $elements = $windows | ForEach-Object { $_.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition) }
}
$button = $elements | Where-Object { $_.Current.AutomationId -eq '1' -and $_.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button } | Select-Object -First 1
$button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
''', encoding='utf-8')
    process = subprocess.Popen([str(product/'narutorise.exe'), '--setup'], cwd=product,
                               env=dict(os.environ, REX_DATA_ROOT=str(product / "data"), REX_USER_DATA_ROOT=str(product/'user')))
    def log():
        files = list((product/'data/logs').glob('*.log'))
        return max(files, key=lambda p:p.stat().st_mtime).read_text(encoding='utf-8', errors='replace') if files else ''
    try:
        hwnd = win.wait_for(lambda:win.window(process.pid), process)
        win.wait_for(lambda:'Setup screen: requested' in log(), process)
        focus.focus_window(hwnd)
        time.sleep(.6)
        def key(value):
            scan = win.user32.MapVirtualKeyW(value, 0)
            flags = 1 | (scan << 16) | ((1 << 24) if 0x21 <= value <= 0x28 else 0)
            win.user32.PostMessageW(hwnd, 0x100, value, flags)
            win.user32.PostMessageW(hwnd, 0x101, value, flags | 0xc0000000)
            time.sleep(.2)
        key(0x28); key(0x28); key(13); key(13)
        time.sleep(.8)
        win.capture(hwnd, output/'before-picker.png')
        subprocess.run(['pwsh','-NoProfile','-File',str(helper),'-AppProcessId',str(process.pid),'-Folder',str(content)], check=True)
        target = product/'user/0000000000000000/555307E5/00000002/sample-dlc/content.bin'
        win.wait_for(lambda:target.exists(), process)
        assert target.read_bytes() == b'dlc integration test'
        time.sleep(.3); win.capture(hwnd, output/'installed.png')
        win.user32.PostMessageW(hwnd, 0x10, 0, 0)
        process.wait(timeout=20); assert process.returncode == 0, process.returncode
        (output/'result.json').write_text(json.dumps(dict(passed=True, exit=process.returncode, content=str(target))), encoding='utf-8')
        print('PASS: native DLC picker, async install, real content path and clean close', flush=True)
    finally:
        if process.poll() is None:
            hwnd = win.window(process.pid)
            if hwnd: win.user32.PostMessageW(hwnd, 0x10, 0, 0)
            try: process.wait(timeout=20)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
        if (product/'data/logs').exists():
            for item in (product/'data/logs').glob('*.log'): shutil.copy2(item, output/item.name)


if __name__ == '__main__': main()
