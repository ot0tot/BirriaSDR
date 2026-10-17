# SPDX-License-Identifier: MIT
"""Bounded spectrum consumer of BirriaSDR's validated native I/Q stream."""
import ctypes as C
from datetime import datetime, timezone
from functools import lru_cache
import json
import os
from pathlib import Path
import struct
import sys
import threading
import time

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
RESOURCE = Path(getattr(sys, '_MEIPASS', Path(__file__).parent))
CALLBACK = C.CFUNCTYPE(None, C.POINTER(C.c_int16), C.c_uint32, C.c_uint64, C.c_void_p)


class Gain(C.Structure):
    _fields_ = [('lna', C.c_uint8), ('lpf', C.c_uint8), ('digital', C.c_uint8)]


class DeviceInfo(C.Structure):
    _fields_ = [('vid', C.c_uint16), ('pid', C.c_uint16), ('bus', C.c_uint8),
                ('address', C.c_uint8), ('chipset', C.c_int),
                ('capabilities', C.c_uint32), ('name', C.c_char * 96)]


class Stats(C.Structure):
    _fields_ = [('samples', C.c_uint64), ('packets', C.c_uint64), ('usb_bytes', C.c_uint64),
                ('firmware_state', C.c_uint32), ('dma_errors', C.c_uint32),
                ('stage', C.c_int), ('reset_error', C.c_int)]


class DisplayBridge:
    """Native, nonblocking producer. Polling and FFT work may stall freely."""
    def __init__(self, directory):
        name = 'birria_display.dll' if os.name == 'nt' else 'libbirria_display.so'
        self.lib = C.CDLL(str(directory / name))
        signatures = {
            'create': ([], C.c_void_p),
            'destroy': ([C.c_void_p], None),
            'configure': ([C.c_void_p, C.c_uint32, C.c_uint64, C.c_int], C.c_int),
            'take': ([C.c_void_p, C.POINTER(C.c_int16), C.c_uint32,
                      C.POINTER(C.c_uint32), C.POINTER(C.c_uint64), C.POINTER(C.c_uint64)], C.c_int),
            'counters': ([C.c_void_p, C.POINTER(C.c_uint64)], None),
            'read': ([C.c_void_p, C.c_void_p], C.c_int),
            'diagnostics_v2': ([C.c_void_p, C.POINTER(C.c_uint64)], C.c_int),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.lib, 'bs_display_' + name)
            fn.argtypes, fn.restype = args, result
            setattr(self, name, fn)
        self.handle = self.create()
        if not self.handle:
            raise MemoryError('Cannot allocate the native display mailbox')
        self.scratch = np.empty((262144, 2), dtype=np.int16)

    def reset(self, size, generation, counters=False):
        if self.configure(self.handle, size, generation, int(counters)):
            raise ValueError('Native display configuration failed')

    def poll(self):
        iq = self.scratch
        count, first, generation = C.c_uint32(), C.c_uint64(), C.c_uint64()
        result = self.take(self.handle, iq.ctypes.data_as(C.POINTER(C.c_int16)), len(iq),
                           C.byref(count), C.byref(first), C.byref(generation))
        if result < 0:
            raise RuntimeError('Native display handoff failed')
        return (generation.value, iq[:count.value].copy(), first.value) if result else None

    def counts(self):
        values = (C.c_uint64 * 5)()
        self.counters(self.handle, values)
        return dict(zip(('samples', 'blocks', 'published', 'busy', 'overwritten'), values))

    def transport(self, dev):
        values = (C.c_uint64 * 10)()
        if self.diagnostics_v2(dev, values):
            return None
        return dict(zip(('queue_peak', 'max_usb_gap_ns', 'usb_completions',
                         'source_backlog', 'source_poll_us', 'queue_capacity',
                         'min_completion_bytes', 'max_completion_bytes',
                         'transfer_bytes', 'aggregation_kib'), values))

    def close(self):
        if self.handle:
            self.destroy(self.handle)
            self.handle = None


@lru_cache(maxsize=4)
def _fft_window(size):
    window = np.hanning(size).astype(np.float32)
    window.flags.writeable = False
    return window, window.sum() * 8192.0


def spectrum(iq, remove_dc=True):
    """Hann-windowed complex tone amplitude, on the native 14-bit scale."""
    z = iq[:, 0].astype(np.float32) + 1j * iq[:, 1].astype(np.float32)
    if remove_dc:
        z -= z.mean()
    window, scale = _fft_window(len(z))
    bins = np.fft.fftshift(np.fft.fft(z * window))
    return (np.abs(bins) / scale) ** 2


class Native:
    def __init__(self, directory=None):
        directory = Path(directory) if directory else RESOURCE / 'native'
        if not directory.exists():
            directory = ROOT / 'build-windows' / 'Release'
        self.directory = directory
        if os.name == 'nt':
            self.dll_directory = os.add_dll_directory(str(directory.resolve()))
            self.usb = C.CDLL(str(directory / 'libusb-1.0.dll'))
            self.lib = C.CDLL(str(directory / 'birriasdr.dll'))
        else:
            self.lib = C.CDLL(str(directory / 'libbirriasdr.so'))
        lib = self.lib
        signatures = {
            'get_device_count': ([], C.c_int),
            'get_device_info': ([C.c_uint32, C.POINTER(DeviceInfo)], C.c_int),
            'open_ex': ([C.POINTER(C.c_void_p), C.c_uint32, C.c_uint32], C.c_int),
            'close': ([C.c_void_p], C.c_int),
            'set_center_freq': ([C.c_void_p, C.c_uint64], C.c_int),
            'set_sample_rate': ([C.c_void_p, C.c_uint32], C.c_int),
            'set_tuner_bandwidth': ([C.c_void_p, C.c_uint32], C.c_int),
            'set_tuner_gain_mode': ([C.c_void_p, C.c_int], C.c_int),
            'set_gain_codes': ([C.c_void_p, C.POINTER(Gain)], C.c_int),
            'read_async': ([C.c_void_p, CALLBACK, C.c_void_p, C.c_uint32, C.c_uint32], C.c_int),
            'cancel_async': ([C.c_void_p], C.c_int),
            'get_stream_stats': ([C.c_void_p, C.POINTER(Stats)], C.c_int),
            'strerror': ([C.c_int], C.c_char_p),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(lib, 'birriasdr_' + name)
            fn.argtypes, fn.restype = args, result
            setattr(self, name, fn)

    def create_bridge(self):
        return DisplayBridge(self.directory)

    def check(self, code):
        if code < 0:
            raise RuntimeError(f'{self.strerror(code).decode()} ({code})')
        return code

    def devices(self):
        rows = []
        for index in range(self.check(self.get_device_count())):
            info = DeviceInfo()
            self.check(self.get_device_info(index, C.byref(info)))
            name = 'MT7961 USB adapter' if (info.vid, info.pid) == (0x0e8d, 0x7961) else info.name.decode(errors='replace')
            rows.append(dict(index=index, name=name,
                             vid=info.vid, pid=info.pid, bus=info.bus, address=info.address))
        return rows


class Receiver:
    def __init__(self, demo=False, native=None):
        self.demo = demo
        self.native = None if demo else native or Native()
        self.bridge = self.native.create_bridge() if not demo and hasattr(self.native, 'create_bridge') else None
        self.control = threading.RLock()
        self.closed = False
        self.lock = threading.Lock()
        self.handle = C.c_void_p()
        self.reader = None
        self.exit = threading.Event()
        self.cancel = threading.Event()
        self.ready = threading.Event()
        self.slot = self.raw = self.frame = None
        self.sequence = self.generation = 0
        self.last_pick = 0.0
        self.status = 'stopped'
        self.error = None
        self.samples = self.blocks = self.display_frames = self.overwritten = 0
        self.started = 0.0
        self.ended = 0.0
        self.final_stats = None
        self.transport_stats = None
        self.error_kind = None
        self.session_path = None
        self.report_write_error = None
        self.settings = dict(index=0, center_hz=2437000000, rate=40000000, bandwidth=40000000,
                             manual=False, lna=2, lpf=4, digital=0, fft_size=65536,
                             averaging=4, dc=True)
        self.callback = CALLBACK(self._callback)
        self.worker = threading.Thread(target=self._fft_worker, name='spectrum', daemon=True)
        self.worker.start()

    def devices(self):
        return [dict(index=0, name='Demo signal generator', vid=0, pid=0)] if self.demo else self.native.devices()

    @staticmethod
    def validate(settings):
        keys = {'index', 'center_hz', 'rate', 'bandwidth', 'manual', 'lna', 'lpf', 'digital', 'fft_size', 'averaging', 'dc'}
        if set(settings) != keys:
            raise ValueError('Unknown or missing receiver setting')
        for key in keys - {'manual', 'dc'}:
            if type(settings[key]) is not int:
                raise ValueError(f'{key} must be an integer')
        if any(type(settings[k]) is not bool for k in ['manual', 'dc']):
            raise ValueError('Gain mode and DC selection must be boolean')
        hz = settings['center_hz']
        if not (1800000000 <= hz <= 3100000000 or 4000000000 <= hz <= 8000000000):
            raise ValueError('Center must be 1800 to 3100 or 4000 to 8000 MHz (exploration bounds)')
        if settings['rate'] not in (40000000, 80000000):
            raise ValueError('Select 40 or 80 MSps')
        if settings['bandwidth'] not in (40000000, 80000000) or settings['bandwidth'] > settings['rate']:
            raise ValueError('The 80 MHz filter requires 80 MSps')
        for key, maximum in [('lna', 3), ('lpf', 8), ('digital', 12), ('index', 255)]:
            if not 0 <= settings[key] <= maximum:
                raise ValueError(f'{key} must be 0 to {maximum}')
        if settings['fft_size'] not in (4096, 16384, 65536, 262144):
            raise ValueError('Unsupported FFT size')
        if not 1 <= settings['averaging'] <= 32:
            raise ValueError('Averaging must be 1 to 32')

    def configure(self, values):
        with self.control:
            if self.closed:
                raise ValueError('Receiver is closed')
            proposed = self.settings | values
            self.validate(proposed)
            changed = {k for k in proposed if proposed[k] != self.settings[k]}
            if not changed:
                return
            live = self.reader is not None and self.reader.is_alive()
            restart = bool(changed & {'index', 'rate', 'bandwidth', 'center_hz'})
            if live and restart:
                self._stop()
            # Gain changes use the native live setters. Frequency/configuration
            # restarts acquisition so an FFT cannot straddle a tuning boundary.
            if live and not restart and not self.demo and changed & {'manual', 'lna', 'lpf', 'digital'}:
                n = self.native
                n.check(n.set_tuner_gain_mode(self.handle, int(proposed['manual'])))
                if proposed['manual']:
                    n.check(n.set_gain_codes(self.handle, C.byref(Gain(proposed['lna'], proposed['lpf'], proposed['digital']))))
            with self.lock:
                self.settings = proposed
                self.generation += 1
                self.slot = self.raw = self.frame = None
                if self.bridge:
                    self.bridge.reset(proposed['fft_size'], self.generation)
            if live and restart:
                self._start()

    def start(self):
        with self.control:
            if self.closed:
                raise ValueError('Receiver is closed')
            if self.reader and self.reader.is_alive():
                return
            self._stop()
            self._start()

    def _start(self):
        s = self.settings.copy()
        self.validate(s)
        self.cancel.clear()
        with self.lock:
            self.status, self.error = 'starting', None
            self.error_kind = None
            self.report_write_error = None
            log_root = Path(sys.executable).parent.parent / 'sessions' if getattr(sys, 'frozen', False) else ROOT / 'build-applications/standalone-sessions'
            self.session_path = log_root / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ') + '.json')
            self.samples = self.blocks = self.display_frames = self.overwritten = 0
            self.started = time.monotonic()
            self.ended = 0.0
            self.last_pick = 0.0
            self.generation += 1
            self.slot = self.raw = self.frame = None
            self.final_stats = None
            self.transport_stats = None
            if self.bridge:
                self.bridge.reset(s['fft_size'], self.generation, counters=True)
        try:
            if not self.demo:
                n = self.native
                n.check(n.open_ex(C.byref(self.handle), s['index'], 7))
                for fn, val in [(n.set_sample_rate, s['rate']), (n.set_tuner_bandwidth, s['bandwidth']),
                                (n.set_center_freq, s['center_hz']), (n.set_tuner_gain_mode, int(s['manual']))]:
                    n.check(fn(self.handle, val))
                if s['manual']:
                    n.check(n.set_gain_codes(self.handle, C.byref(Gain(s['lna'], s['lpf'], s['digital']))))
            self.reader = threading.Thread(target=self._read, name='receiver', daemon=True)
            self.reader.start()
        except Exception as error:
            if self.handle.value:
                self.native.check(self.native.close(self.handle))
                self.handle = C.c_void_p()
            with self.lock:
                self.status, self.error = 'error', str(error)
            raise

    def _callback(self, pointer, count, first, context):
        # No FFT, disk or browser work on the USB thread. Copy at most 20
        # complete windows/second into a one-element mailbox, never an I/Q queue.
        try:
            self.samples += count
            self.blocks += 1
            now = time.monotonic()
            if self.cancel.is_set():
                return
            if now - self.last_pick < .05 or count < self.settings['fft_size']:
                return
            self.last_pick = now
            size = self.settings['fft_size']
            iq = np.ctypeslib.as_array(pointer, shape=(count * 2,)).reshape(-1, 2)[-size:].copy()
            with self.lock:
                if self.slot is not None:
                    self.overwritten += 1
                self.slot = (self.generation, self.settings.copy(), iq, first + count - size)
                self.status = 'running'
            self.ready.set()
        except Exception as error:
            self.error = f'Display callback failed: {error}'
            self.cancel.set()
            if self.handle.value:
                self.native.cancel_async(self.handle)

    def _read(self):
        try:
            if self.demo:
                rng = np.random.default_rng(7961)
                first = 0
                while not self.cancel.wait(.05):
                    s = self.settings.copy()
                    size = s['fft_size']
                    t = np.arange(size)
                    z = (rng.normal(0, 18, size) + 1j * rng.normal(0, 18, size))
                    for offset, amplitude in [(-.23, 1300), (.11, 500), (.36, 170)]:
                        z += amplitude * np.exp(2j * np.pi * offset * t)
                    iq = np.column_stack((z.real, z.imag)).astype(np.int16)
                    self._callback(iq.ctypes.data_as(C.POINTER(C.c_int16)), size, first, None)
                    first += size
            else:
                code = self.bridge.read(self.handle, self.bridge.handle) if self.bridge else self.native.read_async(self.handle, self.callback, None, 0, 262144)
                stats = Stats()
                if self.native.get_stream_stats(self.handle, C.byref(stats)) == 0:
                    self.final_stats = {name: getattr(stats, name) for name, _ in Stats._fields_}
                if self.bridge:
                    self.transport_stats = self.bridge.transport(self.handle)
                if code == -11:
                    state = (self.final_stats or {}).get('firmware_state')
                    diagnostics = self.transport_stats or {}
                    if state in (8, 9):
                        self.error_kind = 'device_overrun'
                        raise RuntimeError(f'Device capture ring overrun (-11). Firmware state {state}.')
                    if diagnostics.get('queue_peak', 0) >= diagnostics.get('queue_capacity', 1):
                        self.error_kind = 'host_overrun'
                        raise RuntimeError(f'Host USB completion queue overrun (-11). Peak {diagnostics["queue_peak"]}/{diagnostics["queue_capacity"]} buffers.')
                self.native.check(code)
        except Exception as error:
            with self.lock:
                self.error = str(error)
        finally:
            with self.lock:
                self.ended = time.monotonic()
                self.status = 'error' if self.error else 'stopped'
            self._save_session()

    def _save_session(self):
        if self.session_path is None:
            return
        try:
            report = self.state() | dict(active=False, finished_utc=datetime.now(timezone.utc).isoformat())
            self.session_path.parent.mkdir(parents=True, exist_ok=True)
            self.session_path.write_text(json.dumps(report, indent=2) + '\n', newline='\n')
        except OSError as error:
            self.report_write_error = str(error)

    def stop(self):
        with self.control:
            self._stop()

    def _stop(self):
        self.cancel.set()
        if self.reader and self.reader.is_alive():
            deadline = time.monotonic() + 20
            # A stop can arrive during native startup, before cancel is armed.
            # Retry cancellation until read_async has returned, without closing
            # a handle underneath either the read thread or control thread.
            while self.reader.is_alive() and time.monotonic() < deadline:
                if not self.demo:
                    self.native.cancel_async(self.handle)
                self.reader.join(.05)
            if self.reader.is_alive():
                raise RuntimeError('Receiver has not stopped. Handle retained until USB cleanup finishes.')
        self.reader = None
        if self.bridge:
            self._sync_counts()
        if self.handle.value:
            self.native.check(self.native.close(self.handle))
            self.handle = C.c_void_p()
        with self.lock:
            self.status = 'error' if self.error else 'stopped'
            self.slot = None
            self.generation += 1
            if self.bridge and self.bridge.handle:
                self.bridge.reset(self.settings['fft_size'], self.generation)

    def _sync_counts(self):
        if not self.bridge.handle:
            return
        counts = self.bridge.counts()
        self.samples, self.blocks = counts['samples'], counts['blocks']
        self.overwritten = counts['overwritten']

    def _fft_worker(self):
        previous = None
        generation = -1
        while not self.exit.is_set():
            self.ready.wait(.01 if self.bridge else .2)
            self.ready.clear()
            with self.lock:
                slot, self.slot = self.slot, None
            if self.bridge:
                window = self.bridge.poll()
                if window is not None:
                    gen, iq, first = window
                    with self.lock:
                        if gen != self.generation:
                            continue
                        settings = self.settings.copy()
                    slot = (gen, settings, iq, first)
            if slot is None:
                continue
            gen, settings, iq, first = slot
            try:
                power = spectrum(iq, settings['dc'])
                if generation != gen:
                    previous = None
                    generation = gen
                amount = 1.0 / settings['averaging']
                previous = power if previous is None else previous * (1 - amount) + power * amount
                db = (10 * np.log10(np.maximum(previous, 1e-16))).astype('<f4')
                with self.lock:
                    if gen != self.generation:
                        continue
                    self.sequence += 1
                    # 40-byte LE header: magic, sequence, bin count, rate,
                    # center Hz, first complex sample, UNIX seconds, then f32 dBFS.
                    self.frame = struct.pack('<4sIIIQQd', b'BFFT', self.sequence, len(db),
                                             settings['rate'], settings['center_hz'], first, time.time()) + db.tobytes()
                    self.raw = (iq, settings.copy(), first)
                    self.display_frames += 1
                    if self.reader and self.reader.is_alive() and not self.error:
                        self.status = 'running'
            except Exception as error:
                with self.lock:
                    self.error = f'Spectrum processing failed: {error}'
                self.cancel.set()
                if self.handle.value:
                    self.native.cancel_async(self.handle)

    def state(self):
        with self.lock:
            if self.bridge:
                self._sync_counts()
            elapsed = max(.001, (self.ended or time.monotonic()) - self.started) if self.started else 0
            active = bool(self.reader and self.reader.is_alive())
            return dict(mode='demo' if self.demo else 'hardware', status=self.status, error=self.error,
                        active=active, settings=self.settings.copy(), samples=self.samples, blocks=self.blocks,
                        display_frames=self.display_frames, display_overwrites=self.overwritten,
                        elapsed_s=elapsed, average_msps=self.samples / max(elapsed, .001) / 1e6,
                        sequence=self.sequence, final_stats=self.final_stats, transport_stats=self.transport_stats,
                        display_handoff='native' if self.bridge else 'synthetic',
                        error_kind=self.error_kind, diagnostics_path=str(self.session_path) if self.session_path else None,
                        report_write_error=self.report_write_error,
                        tuning_ranges_hz=[[1800000000, 3100000000], [4000000000, 8000000000]])

    def snapshot(self):
        with self.lock:
            if self.error:
                raise ValueError('Capture failed. Restart reception before saving a snapshot.')
            if self.raw is None:
                raise ValueError('Start reception and wait for a spectrum frame first')
            iq, settings, first = self.raw
        metadata = dict(format='ci16_le', component_scale=8192, samples=len(iq),
                        first_sample=first, center_hz=settings['center_hz'], sample_rate=settings['rate'],
                        bandwidth=settings['bandwidth'], settings=settings,
                        scope='One contiguous displayed FFT window, not a continuous recording',
                        mode='demo' if self.demo else 'hardware')
        return iq.astype('<i2', copy=False).tobytes(), metadata

    def close(self):
        with self.control:
            if self.closed:
                return
            self.stop()
            self.exit.set()
            self.ready.set()
            self.worker.join(3)
            if self.worker.is_alive():
                raise RuntimeError('Display worker has not stopped')
            # HTTP state readers also take this lock while reading counters.
            # Destroy only after their native calls and both workers finish.
            with self.lock:
                if self.bridge:
                    self.bridge.close()
                self.closed = True
