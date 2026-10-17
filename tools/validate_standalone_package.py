# SPDX-License-Identifier: MIT
"""Short capture/exit acceptance check of the final portable executable."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--executable', type=Path, default=ROOT / 'build-applications/standalone/BirriaSDR/BirriaSDR.exe')
parser.add_argument('--demo', action='store_true')
parser.add_argument('--seconds', type=float, default=5)
parser.add_argument('--fft-size', type=int, default=65536, choices=[4096, 16384, 65536, 262144])
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
exe = args.executable.resolve()
report = dict(executable=str(exe), executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
p = subprocess.Popen([str(exe), '--no-browser', '--port', '0'] + (['--demo'] if args.demo else []), stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, text=True,
                     creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
url = token = None
try:
    line = p.stdout.readline()
    report['startup'] = line
    assert line.startswith('BirriaSDR DEMO: ' if args.demo else 'BirriaSDR MT7961: '), line
    url = line.strip().split(': ', 1)[1]
    def get(path):
        with urllib.request.urlopen(url + path, timeout=20) as r:
            return json.load(r)
    token = get('/api/state')['token']
    def post(path, values=None):
        request = urllib.request.Request(url + path, data=json.dumps(values or {}).encode(),
                                         headers={'X-Birria-Token': token, 'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=30) as r:
            return json.load(r)
    post('/api/configure', dict(rate=80000000, bandwidth=80000000, center_hz=5800000000, fft_size=args.fft_size,
                               manual=True, lna=3, lpf=4, digital=0))
    post('/api/start')
    time.sleep(args.seconds)
    active = get('/api/state');active.pop('token', None)
    report['active'] = active
    assert active['active'] and not active['error'] and active['samples'] > (0 if args.demo else 200000000), active
    if not args.demo:
        assert active['display_handoff'] == 'native', active
    report['final'] = post('/api/stop')
    assert not report['final']['error'] and not report['final']['active'], report['final']
    if not args.demo:
        assert report['final']['final_stats']['dma_errors'] == 0
        assert report['final']['transport_stats']['queue_peak'] < report['final']['transport_stats']['queue_capacity']
    post('/api/shutdown')
    assert p.wait(10) == 0
    report['exit_code'] = p.returncode
    report['passed'] = True
finally:
    if p.poll() is None:
        if url and token:
            try:post('/api/shutdown')
            except Exception:pass
        try:p.wait(10)
        except subprocess.TimeoutExpired:p.terminate();p.wait(5)
    report['remaining_output'] = p.stdout.read()
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', newline='\n')
print(json.dumps(report, indent=2))
