"""Exercise the Release EXE in isolation; never use personal config or models."""
import argparse
import hashlib
import http.client
import json
import os
import re
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

from test_startup_latency import ROOT, RELEASE, process_windows, user32, remove_test_firewall_rule


def request(host, port, path='/', body=None):
    connection = http.client.HTTPConnection(host, port, timeout=3)
    try:
        connection.request('POST' if body is not None else 'GET', path,
                           body.encode('utf-8') if body is not None else None)
        response = connection.getresponse()
        data = response.read()
        return response.status, response.getheader('Content-Type'), data
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--lan-ip', required=True)
    parser.add_argument('--hold', action='store_true', help='Keep the scratch server for browser checks')
    parser.add_argument('--occupy-port-80', action='store_true', help='Verify fallback to explicit port')
    parser.add_argument('--occupy-primary-ports', type=int, choices=[0, 1, 11], default=0)
    parser.add_argument('--web-panel', type=Path, default=ROOT / 'web' / '圣人视觉识别系统.html')
    args = parser.parse_args()
    scratch = (ROOT / '.workbuddy').resolve()
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='lan-reload-', dir=scratch) as temp:
        directory = Path(temp).resolve()
        assert directory.is_relative_to(scratch)
        exe = directory / '圣人双机服务端.exe'
        shutil.copyfile(RELEASE / exe.name, exe)
        for library in RELEASE.glob('*.dll'):
            os.link(library, directory / library.name)
        shutil.copyfile(ROOT / '圣人自用.sample.js', directory / '圣人自用.js')
        (directory / '主机IP.ini').write_text('203.0.113.77', encoding='ascii')
        (directory / 'mouse.bin').write_bytes(bytes(11728))
        data_dir = directory / '人手数据'
        data_dir.mkdir()
        row = ','.join('"%d,%d"' % (i, -i) for i in [9] + list(range(10)))
        (data_dir / '人手数据.txt').write_text((row + '\n') * 4, encoding='ascii')
        log_file = directory / '运行日志.txt'
        occupied = None
        occupied_primary = []
        for occupied_port in range(8888, 8888 + args.occupy_primary_ports):
            listener = socket.socket()
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            listener.bind(('0.0.0.0', occupied_port))
            listener.listen()
            occupied_primary.append(listener)
        if args.occupy_port_80:
            occupied = socket.socket()
            occupied.bind(('0.0.0.0', 80))
            occupied.listen()
        process = subprocess.Popen([str(exe)], cwd=os.environ['SystemRoot'])
        observed = set()
        window = None
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and process.poll() is None:
                for hwnd, name in process_windows(process.pid):
                    observed.add(name)
                    if name == 'SagaAppWindow':
                        window = hwnd
                log = log_file.read_text(encoding='utf-8-sig', errors='replace') if log_file.exists() else ''
                if '[web] LAN listener ready' in log and '[12]' in log:
                    break
                time.sleep(.05)
            assert window and 'SagaAppInput' not in observed and '#32770' not in observed, observed
            assert '[web] LAN listener ready' in log, log
            match = re.search(r'LAN listener ready: [\d.]+:(\d+), port80=(\w+)', log)
            assert match, log
            primary_port = int(match[1])
            standard_port = match[2] == 'ready'
            if occupied:
                assert not standard_port
            if occupied_primary:
                assert primary_port not in range(8888, 8888 + args.occupy_primary_ports)
            port = 80 if standard_port else primary_port
            health = request('127.0.0.1', primary_port, '/__saga_health')
            assert health[0] == 200 and json.loads(health[2]) == {'service': 'SagaApp', 'pid': process.pid}
            deadline = time.monotonic() + 10
            report = ''
            while time.monotonic() < deadline:
                if log_file.exists():
                    report = log_file.read_text(encoding='utf-8-sig', errors='replace')
                if '本机 HTTP 自检：通过' in report:
                    break
                time.sleep(.1)
            assert '本机 HTTP 自检：通过' in report, report
            assert f'http://127.0.0.1:{primary_port}/' in report and '网页不存在' in report, report
            assert '防火墙预设：已配置' in report and f'TCP {primary_port}' in report, report
            for obsolete in ['SagaApp_startup.log', 'Makcu驱动报告.txt', 'Makcu串口日志.txt', '网络自检.txt']:
                assert not (directory / obsolete).exists(), obsolete
            print(f'PASS: current-process HTTP probe and background firewall preflight; actual port {primary_port}.', flush=True)
            expected = args.web_panel.read_bytes()
            panel = directory / '圣人视觉识别系统.html'
            # Start with no HTML: no bundled fallback, and tuning stays disabled.
            before = (directory / '圣人自用.js').read_bytes()
            for host in ['127.0.0.1', args.lan_ip]:
                for test_port in ([80, primary_port] if standard_port else [primary_port]):
                    code, content_type, missing = request(host, test_port)
                    assert code == 404 and 'text/html' in content_type
                    assert '网页不存在' in missing.decode('utf-8')
            assert request(args.lan_ip, port, '/api', '置信度|0.99')[0] == 404
            assert request(args.lan_ip, port, '/config.js')[0] == 404
            assert (directory / '圣人自用.js').read_bytes() == before
            panel.write_bytes(expected)
            for host in ['127.0.0.1', args.lan_ip]:
                for test_port in ([80, primary_port] if standard_port else [primary_port]):
                    code, content_type, page = request(host, test_port)
                    assert code == 200 and 'text/html' in content_type
                    assert hashlib.sha256(page).digest() == hashlib.sha256(expected).digest()
                    assert json.loads(request(host, test_port, '/api')[2])['推理帧率'] == '0'
            # Edits and removal are reflected immediately, without restarting.
            marker = b'\n<!-- external-panel-refresh-test -->'
            panel.write_bytes(expected + marker)
            assert request(args.lan_ip, port)[2] == expected + marker
            panel.unlink()
            before = (directory / '圣人自用.js').read_bytes()
            assert request(args.lan_ip, port)[0] == 404
            assert request(args.lan_ip, port, '/api', '置信度|0.99')[0] == 404
            assert (directory / '圣人自用.js').read_bytes() == before
            panel.write_bytes(expected)
            code, content_type, script = request(args.lan_ip, port, '/config.js')
            assert code == 200 and 'javascript' in content_type
            assert '"Kp":0' in script.decode('utf-8')
            assert request(args.lan_ip, port, '/api', '置信度|0.73')[0] == 201
            assert '"置信度":0.73' in request(args.lan_ip, primary_port, '/config.js')[2].decode('utf-8')
            assert '置信度=0.73' in (directory / '圣人自用.js').read_text(encoding='utf-8')
            before = (directory / '圣人自用.js').read_bytes()
            assert request(args.lan_ip, port, '/api', '导入配置|nonexistent')[0] == 400
            assert request(args.lan_ip, port, '/api', '删除配置|../outside')[0] == 400
            assert (directory / '圣人自用.js').read_bytes() == before
            assert request(args.lan_ip, port, '/unknown')[0] == 404
            trained = subprocess.run([str(exe), '--train-human-data', '--data-dir', str(data_dir),
                                      '--epochs', '2', '--no-pause'], capture_output=True, timeout=15)
            assert trained.returncode == 0, trained.stdout.decode('utf-8', errors='replace')
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                log = log_file.read_text(encoding='utf-8-sig', errors='replace')
                if '[traj] model hot reload: success' in log:
                    break
                time.sleep(.05)
            assert '[traj] model hot reload: success' in log, log
            assert (data_dir / 'mouse.bin').stat().st_size == 11728 and process.poll() is None
            print('PASS: missing page returns 404 and disables tuning; supplied HTML loads from EXE directory; '
                  'page updates/removal take effect immediately; no IP prompt; '
                  'parameter update persists; training reloads without restarting.', flush=True)
            if occupied:
                print('PASS: occupied port 80 preserves the listener on its explicit port.', flush=True)
            if occupied_primary:
                print(f'PASS: {len(occupied_primary)} occupied primary ports recovered automatically.', flush=True)
            if args.hold:
                ready = scratch / 'lan-check-ready.json'
                stop = directory / 'stop-browser-check'
                ready.write_text(json.dumps({'directory': str(directory), 'pid': process.pid,
                                            'stop_file': str(stop), 'url': f'http://{args.lan_ip}:{port}'}), encoding='utf-8')
                print('BROWSER_READY=' + str(ready), flush=True)
                deadline = time.monotonic() + 300
                while not stop.exists() and time.monotonic() < deadline and process.poll() is None:
                    time.sleep(.2)
                ready.unlink(missing_ok=True)
        finally:
            if process.poll() is None:
                if window:
                    user32.PostMessageW(window, 0x0010, 0, 0)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=3)
            if occupied:
                occupied.close()
            for listener in occupied_primary:
                listener.close()
            remove_test_firewall_rule(exe)


if __name__ == '__main__':
    main()
