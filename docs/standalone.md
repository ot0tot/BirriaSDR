# Standalone spectrum receiver

The standalone app runs the MT7961 through our native driver and shows its
spectrum, waterfall and receiver controls in your browser. The Windows package
includes its own Python runtime and NumPy. You do not need SDR# or an installed
Python interpreter. The layout follows the browser receiver approach used by
the [W1700K SDR](https://github.com/h0m3us3r/w1700k-sdr) project. Our app uses
original frontend and server code with the existing BirriaSDR USB backend.

## Run the app

Extract the complete `BirriaSDR-standalone-windows-x64.zip` archive, then run
`BirriaSDR/BirriaSDR.exe`. Keep its `_internal` folder next to the executable.
The app opens your browser at `http://127.0.0.1:8073`. Select the attached
receiver and press **Start receiver**. The [Windows driver setup](windows.md)
still applies. Close other programs using the adapter first.
The app selects an available port if the requested one is already in use.

The server listens only on the local loopback address. The **Stop receiver**
button cancels the native read, waits for USB cleanup and closes the handle.
The **Exit app** button stops reception and exits the server. Closing only the
browser tab leaves the server and any active reception running. You can reopen
the local address. The server console also accepts Ctrl+C to stop and exit.

You can choose another port or run a synthetic demonstration:

```powershell
.\BirriaSDR.exe --port 8074
.\BirriaSDR.exe --demo
```

The demonstration uses generated samples and does not open USB. Its visible
badge and snapshot metadata identify synthetic data. The `--no-browser` option
leaves browser launch to you. The `--port 0` option chooses an available port
and prints its address in the console.

## Tune and inspect

Enter a center frequency in MHz and press Enter. The step buttons use the
selected tuning step. The preset buttons select 2437, 5800 or 6000 MHz.
Double-click the spectrum or waterfall to center the receiver on that frequency.
Scroll over either plot to zoom around the pointer and drag to pan within the
captured span. The **Full span** button restores the complete sample span.

The rate selector offers 40 and experimental 80 MSps. The analog filter
selector offers nominal 40 and experimental 80 MHz profiles. The wider filter
requires 80 MSps. These settings do not establish a flat usable passband.
Read the [bandwidth measurements](bandwidth-validation.md) for the measured
rolloff and aliases. The tuning windows are 1800 to 3100 and 4000 to 8000 MHz.
These are programming bounds, not guaranteed reception across either window.
The gap between the windows is rejected before changing the receiver.

Changing frequency, device, rate or bandwidth stops the current acquisition
and starts another one. This avoids labeling queued samples with a new center
frequency. The app does not claim gapless or phase-continuous tuning.
Gain changes use the native live control path. Manual gain exposes LNA codes
from zero to three, filter gain from zero to eight and digital gain from zero
to twelve. The values are raw hardware codes, not dB. Releasing manual
overrides is not qualified global AGC control.

## Read the display

The spectrum uses a Hann window and a complex FFT. It scales the native signed
14-bit components by 8192. A bin-centered complex sinusoid of amplitude 4096
appears near −6.02 dBFS. The display is a tone amplitude spectrum, not a
calibrated power density measurement. The averaging control averages linear
power across displayed frames. The peak hold trace is retained in the browser.

The native callback drains every validated sample block without entering
Python. It copies at most twenty complete FFT windows per second into a
bounded native mailbox for display. The largest 262,144-point FFT is limited
to ten windows per second to reduce host load. The producer never waits for
the display consumer. A busy mailbox skips a display window while sample
draining continues.
The spectrum therefore samples the stream in time. It can miss short bursts
between displayed windows and does not transform every received sample.
Display frame overwrites do not represent dropped USB samples. The transport
counter reports samples delivered by the native API. Final capture success
and native statistics are available after stopping.

The **Remove FFT DC offset** control subtracts each selected window's complex
mean before its FFT. It affects only the display. It does not remove broad
center pedestals, fixed RF spurs or all I/Q imbalance. The original snapshot
samples are unchanged. Passband flattening has not been calibrated and is
not offered.

The **Save I/Q snapshot** button downloads a ZIP with `capture.cs16` and
`capture.json`. The sample file contains one contiguous displayed FFT window
of interleaved little-endian signed 16-bit I/Q on the 14-bit scale. The JSON
records its frequency, rate, gain settings and first delivered sample index.
The snapshot is not a continuous recorder. A saved demo snapshot is marked
as synthetic.

## Diagnose capture errors

The **Save diagnostics** button downloads the current settings and stream
statistics. Every completed or failed capture also writes a JSON report in
the `sessions` directory beside the packaged `BirriaSDR` directory. The
reported diagnostics path identifies its exact location. A host queue overrun
and a device ring overrun are distinguished when the native evidence permits
it. An overrun still stops capture and invalidates continuity. The app does
not silently discard received samples or splice separate captures together.

The current package uses 256 KiB host reads and 232 KiB aggregation at
80 MSps. This reduces request turnover compared with the earlier 64 KiB
reads and 56 KiB aggregation. Diagnostics include these sizes and the actual
completion lengths. A large FFT can expose transport timing limits under host
load even when the host queue has space. The device still has the same capture
ring and overrun guard. Use 40 MSps if device overruns persist. The
[transport notes](usb-streaming.md#device-overrun-with-large-ffts) separate the
reproduced failure from the measured improvements.

## Build and validate

Run the app from the source checkout with Python 3 and NumPy:

```powershell
.\birria-sdr.cmd
python integrations/standalone/app.py --demo
```

The native DLLs default to `build-windows/Release`. The Windows package build
uses the existing native build script and a project-local PyInstaller install:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-standalone.ps1
python -m unittest discover -s tests -p test_standalone.py -v
```

The package includes runtime license notices and the corresponding source of
the pinned libusb release. The focused tests drive synthetic tones, tuning
bounds, native lifecycle order, local HTTP control restrictions and snapshots.
The browser validation runner exercises the visible controls and saves
screenshots. Hardware validation is recorded separately from these tests.
