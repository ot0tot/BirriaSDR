# Windows setup

The tested adapter identifies as USB `0e8d:7961`, with digital die `0x7961`
and analog die `0x79728a10`. The Windows applications enable its experimental
board profile automatically. The upstream COMFAST `3574:6211` profile remains
available through the library. Other boards with the same chipset are not
qualified by these tests.

## Prepare the adapter

Connect the adapter directly to a USB 3 port. A USB 2 connection cannot carry
the default stream. Avoid sharing a busy hub with storage or another receiver.

Open Zadig, enable **Options > List All Devices** and select
**WiFi_If (Interface 3)** for `0e8d:7961`. Install **libusbK** on that interface.
Keep the composite parent and Bluetooth interfaces unchanged. This replaces
the Wi-Fi driver for that interface, so the adapter cannot provide ordinary
Wi-Fi networking while configured this way. You can restore the network
driver through Windows Device Manager later.

Reconnect the adapter after changing its driver. The receiver needs a cold
device without a vendor ROM patch already loaded. All receiver configuration
is volatile. The software writes neither flash nor eFuse.

The required host-wrapper read uses request type `0xde`. WinUSB rejected it
with Windows error 87 in tests with libusb 1.0.24 and 1.0.30. Use libusbK.
The packages supply a pinned libusb 1.0.30 runtime. Do not substitute an older
copy from another SDR application.

## Run a packaged application

Extract the whole release archive before running it. Open
`standalone/BirriaSDR.exe` for the browser receiver. The standalone archive
also works independently. Read the [receiver guide](standalone.md) for controls.
Close other applications using the adapter before starting reception.

The [application guide](applications.md) explains installation into an existing
compatible SDR# directory. The plugin does not require a custom SDR# install.
It currently supports the checked x64 .NET 8 production build 1921. Other
versions require separate source-registration validation.

## Build from source

Install Visual Studio 2022 or newer C++ Build Tools with CMake. Run this command
from the repository root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-windows.ps1
.\build-windows\Release\birria_rx.exe --list
.\build-windows\Release\birria_rx.exe --experimental-mt7961 capture.cs16 8000000 manual
```

The script downloads the official x64 libusb 1.0.30 archive and checks its
SHA-256. The build runs the offline native tests. The command above records
eight million complex samples at the default 2437 MHz and 40 MSps. Check its
exit status before using the file. A failed capture can leave a partial file.

The optional preflight command reports the Windows interface driver:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/windows-preflight.ps1
```

The process-scoped execution-policy option lets these commands run without
changing the machine's policy. A packaged executable needs no Python install.
Building the standalone app needs Python 3.12 x64 with NumPy and pip:

```powershell
python -m pip install numpy==2.3.5
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-standalone.ps1
```

The `-Python` parameter selects an interpreter when `python` is not on PATH.
The [application guide](applications.md) covers the optional SDR# and Soapy builds.

## Recover from an error

Stop the receiver and save diagnostics before retrying. An overrun means
sample continuity was lost. The software reports the failure rather than
silently joining separate acquisitions. Try 40 MSps if 80 MSps fails under
host load. Read the [transport limits](usb-streaming.md) for the distinction
between display skips, host queue exhaustion and device ring overruns.

Close the application before reconnecting a faulted adapter. Some USB failures
remain unrecoverable until a physical reconnect. Repeated software resets and
clearing endpoint stalls did not recover every observed failure.
