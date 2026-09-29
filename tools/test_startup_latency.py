"""Check the real application's unplugged-device startup in a scratch directory.

Run on Windows with no Makcu box attached. Uses only Python's standard library;
Python is a test runner, not a dependency of the application.
"""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
RELEASE = ROOT / 'x64' / 'Release'
user32 = ctypes.WinDLL('user32', use_last_error=True)
callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]


def process_windows(pid):
    windows = []

    @callback_type
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            name = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, name, len(name))
            windows.append((hwnd, name.value))
        return True

    user32.EnumWindows(visit, 0)
    return windows


def remove_test_firewall_rule(exe):
    assert Path(exe).resolve().is_relative_to((ROOT / '.workbuddy').resolve())
    environment = dict(os.environ, SAGA_TEST_EXE=str(exe))
    subprocess.run(['powershell.exe', '-NoProfile', '-Command',
                    "try { $rules = (New-Object -ComObject HNetCfg.FwPolicy2).Rules; "
                    "$rules.Remove('SagaApp LAN Web - ' + $env:SAGA_TEST_EXE) } catch {}"],
                   env=environment, capture_output=True, timeout=10,
                   creationflags=subprocess.CREATE_NO_WINDOW)


def main():
    scratch_root = ROOT / '.workbuddy'
    scratch_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='startup-check-', dir=scratch_root) as temp:
        directory = Path(temp).resolve()
        assert directory.is_relative_to(scratch_root.resolve())
        exe = directory / '圣人双机服务端.exe'
        shutil.copyfile(RELEASE / exe.name, exe)
        for library in RELEASE.glob('*.dll'):
            os.link(library, directory / library.name)
        # A valid synthetic model avoids a first-use prompt. Never read personal data.
        (directory / 'mouse.bin').write_bytes(bytes(11728))
        started = time.perf_counter()
        process = subprocess.Popen([str(exe)], cwd=os.environ['SystemRoot'])
        window, visible_after = None, None
        observed = set()
        log = ''
        try:
            deadline = started + 5
            while time.perf_counter() < deadline and process.poll() is None:
                for hwnd, name in process_windows(process.pid):
                    observed.add(name)
                    if name == 'SagaAppWindow' and window is None:
                        window = hwnd
                        visible_after = time.perf_counter() - started
                log_path = directory / 'SagaApp_startup.log'
                if log_path.exists():
                    log = log_path.read_text(encoding='gbk', errors='replace')
                if window and '[06]' in log and '[12]' in log:
                    break
                time.sleep(.01)
            assert window, f'Main window not visible within five seconds:\n{log}'
            assert visible_after < 3, f'Main window took {visible_after:.3f}s'
            assert 'CH343 async/skip-absent v4' in log, 'Wrong application build'
            assert '跳过安装、硬件扫描及串口等待' in log, 'This regression check requires an unplugged box'
            assert '[06] 无可用盒子串口，跳过连接等待' in log
            assert 'official DRVSETUP64' not in log and 'pnputil' not in log
            assert not (directory / 'Makcu驱动报告.txt').exists()
            assert '#32770' not in observed, f'Unexpected message box: {observed}'
            assert 'SagaAppInput' not in observed, 'Startup must not ask for an IP address'
            print(json.dumps({'visible_seconds': round(visible_after, 3),
                              'visible_window_classes': sorted(observed),
                              'startup_steps': [line for line in log.splitlines()
                                                if '[08]' in line or '[06]' in line]}, ensure_ascii=True))
            print('PASS: main window appears promptly; no driver reinstall, diagnostic subprocess, connection wait or warning dialog.')
        finally:
            if process.poll() is None:
                if window:
                    user32.PostMessageW(window, 0x0010, 0, 0)  # WM_CLOSE, only our test process.
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=3)
            remove_test_firewall_rule(exe)


if __name__ == '__main__':
    main()
