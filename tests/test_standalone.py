"""DSP, HTTP control and native lifecycle checks without USB access."""
import ctypes as C
import http.client
import io
import json
from pathlib import Path
import struct
import sys
import threading
import time
import unittest
import zipfile

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'integrations/standalone'))
from receiver import Receiver, Stats, spectrum
from app import Server


def wait_for(predicate, seconds=4):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.01)
    raise AssertionError('Deadline expired')


class SpectrumTests(unittest.TestCase):
    def test_complex_tone_sign_scale_and_dc(self):
        size, bin_index = 16384, -701
        z = 4096 * np.exp(2j * np.pi * bin_index * np.arange(size) / size) + 250 + 100j
        iq = np.rint(np.column_stack((z.real, z.imag))).astype(np.int16)
        power = spectrum(iq)
        self.assertEqual(int(np.argmax(power)), size // 2 + bin_index)
        self.assertAlmostEqual(float(10 * np.log10(power.max())), -6.0206, places=2)
        self.assertLess(float(power[size // 2]), 1e-12)
        uncorrected = spectrum(iq, False)
        self.assertGreater(float(uncorrected[size // 2]), 1e-4)

    def test_command_bounds_and_gap_are_enforced(self):
        r = Receiver(demo=True)
        try:
            for values in [dict(center_hz=3500000000), dict(center_hz=8000000001),
                           dict(bandwidth=80000000), dict(lna=4), dict(rate=160000000),
                           dict(fft_size=8192), dict(dc='false'), dict(index=-1), dict(unknown=2)]:
                with self.assertRaises(ValueError, msg=str(values)):
                    r.configure(values)
            r.configure(dict(center_hz=8000000000, rate=80000000, bandwidth=80000000))
        finally:
            r.close()


class FakeNative:
    def __init__(self, error=False):
        self.cancelled = threading.Event()
        self.entered = threading.Event()
        self.reading = False
        self.closed = 0
        self.error = error
        self.live_gains = []

    def check(self, value):
        if value < 0:
            raise RuntimeError(f'Native error {value}')
        return value

    def open_ex(self, pointer, index, flags):
        C.cast(pointer, C.POINTER(C.c_void_p))[0] = C.c_void_p(123)
        self.cancelled.clear()
        return 0

    def close(self, handle):
        if self.reading:
            raise AssertionError('Closed during active read')
        self.closed += 1
        return 0

    def set_center_freq(self, *args): return 0
    def set_sample_rate(self, *args): return 0
    def set_tuner_bandwidth(self, *args): return 0
    def set_tuner_gain_mode(self, *args): return 0
    def set_gain_codes(self, handle, gain):
        self.live_gains.append(C.cast(gain, C.POINTER(C.c_uint8))[0])
        return 0

    def read_async(self, handle, callback, context, depth, count):
        self.reading = True
        self.entered.set()
        try:
            iq = np.zeros((count, 2), np.int16)
            callback(iq.ctypes.data_as(C.POINTER(C.c_int16)), count, 0, None)
            if self.error:
                return -11
            self.cancelled.wait(4)
            return 0
        finally:
            self.reading = False

    def cancel_async(self, *_):
        self.cancelled.set()
        return 0

    def get_stream_stats(self, *_): return 0


class LifecycleTests(unittest.TestCase):
    def test_native_overrun_cause_and_saved_diagnostics(self):
        class Mailbox:
            handle = 1
            def reset(self, *_args, **_kwargs): pass
            def poll(self): return None
            def read(self, *_): return -11
            def counts(self): return dict(samples=0, blocks=0, overwritten=0)
            def transport(self, _): return dict(queue_peak=2048, queue_capacity=2048)
            def close(self): self.handle = None
        class OverrunNative(FakeNative):
            def create_bridge(self): return Mailbox()
            def get_stream_stats(self, handle, pointer):
                C.cast(pointer, C.POINTER(Stats)).contents.firmware_state = self.firmware
                return 0
        for firmware, kind in [(0, 'host_overrun'), (9, 'device_overrun')]:
            n = OverrunNative();n.firmware = firmware
            r = Receiver(native=n)
            try:
                r.start()
                wait_for(lambda: r.state()['status'] == 'error')
                r.stop()
                self.assertEqual(r.state()['error_kind'], kind)
                saved = json.loads(r.session_path.read_text())
                self.assertEqual(saved['error_kind'], kind)
                self.assertEqual(saved['transport_stats']['queue_peak'], 2048)
                self.assertFalse(saved['active'])
            finally:
                r.close()

    def test_live_gain_and_restarted_tuning_release_in_order(self):
        n = FakeNative()
        r = Receiver(native=n)
        try:
            r.start()
            wait_for(lambda: r.frame is not None)
            r.configure(dict(manual=True, lna=3))
            self.assertEqual(n.closed, 0)
            self.assertEqual(n.live_gains, [3])
            r.configure(dict(center_hz=5800000000))
            self.assertEqual(n.closed, 1)
            wait_for(lambda: r.frame is not None)
            self.assertEqual(struct.unpack_from('<Q', r.frame, 16)[0], 5800000000)
            r.stop()
            self.assertFalse(n.reading)
            self.assertEqual(n.closed, 2)
            self.assertFalse(r.handle.value)
            r.close()
            self.assertFalse(r.state()['active'])
            with self.assertRaisesRegex(ValueError, 'closed'):
                r.start()
            with self.assertRaisesRegex(ValueError, 'closed'):
                r.configure(dict(lna=1))
        finally:
            r.close()

    def test_capture_error_remains_visible_and_handle_is_released(self):
        n = FakeNative(error=True)
        r = Receiver(native=n)
        try:
            r.start()
            wait_for(lambda: r.state()['status'] == 'error')
            self.assertIn('-11', r.state()['error'])
            with self.assertRaisesRegex(ValueError, 'Capture failed'):
                r.snapshot()
            r.stop()
            self.assertEqual(n.closed, 1)
            self.assertEqual(r.state()['status'], 'error')
        finally:
            r.close()


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.receiver = Receiver(demo=True)
        self.server = Server(('127.0.0.1', 0), self.receiver)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.receiver.close()

    def request(self, method, path, body=None, token=True, headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=5)
        h = {'X-Birria-Token': self.server.token} if token else {}
        h.update(headers or {})
        try:
            connection.request(method, path, json.dumps(body or {}) if method == 'POST' else None, h)
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    def test_local_controls_and_binary_snapshot(self):
        self.assertEqual(self.request('POST', '/api/start', token=False)[0], 403)
        self.assertEqual(self.request('GET', '/api/state', headers={'Host': 'attacker.example'})[0], 403)
        self.assertEqual(self.request('POST', '/api/start', headers={'Origin': 'https://example.org'})[0], 403)
        self.assertEqual(self.request('POST', '/api/configure', dict(center_hz=3500000000))[0], 400)
        self.assertEqual(self.request('POST', '/api/start')[0], 200)
        wait_for(lambda: self.receiver.frame is not None)
        code, data = self.request('GET', '/api/frame')
        self.assertEqual(code, 200)
        magic, _, size, rate, center, _, _ = struct.unpack('<4sIIIQQd', data[:40])
        self.assertEqual((magic, size, rate, center), (b'BFFT', 65536, 40000000, 2437000000))
        self.assertEqual(len(data), 40 + size * 4)
        code, snapshot = self.request('POST', '/api/snapshot')
        self.assertEqual(code, 200)
        with zipfile.ZipFile(io.BytesIO(snapshot)) as z:
            metadata = json.loads(z.read('capture.json'))
            self.assertEqual(metadata['mode'], 'demo')
            self.assertEqual(len(z.read('capture.cs16')), size * 4)
        self.assertEqual(self.request('POST', '/api/stop')[0], 200)
        self.assertFalse(self.receiver.state()['active'])
        _, frame = self.request('GET', '/api/frame')
        sequence = struct.unpack_from('<I', frame, 4)[0]
        self.assertEqual(self.request('GET', f'/api/frame?after={sequence}'), (204, b''))
        self.assertEqual(self.request('GET', f'/api/frame?after={sequence-1}')[0], 200)
        self.assertEqual(self.request('GET', '/api/frame?after=invalid')[0], 400)


if __name__ == '__main__':
    unittest.main()
