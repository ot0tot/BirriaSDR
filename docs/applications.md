# Receiver applications

The fork includes a standalone spectrum receiver, an SDR# source plugin and
an optional SoapySDR receive module. All use the same native receive library.
Start with the [Windows driver setup](windows.md). The package contains no
transmit mode.

## Use the standalone receiver

Run `standalone/BirriaSDR.exe` from the combined package, or
`BirriaSDR/BirriaSDR.exe` from the standalone archive. The application opens a
local browser with spectrum, waterfall, frequency, gain and snapshot controls.
Read the [standalone guide](standalone.md) for usage and diagnostics.

## Add the SDR# source

Use your existing compatible SDR# installation. The checked host is the x64
.NET 8 production build 1921, with `SDRSharp.dotnet8.exe` and `SDRSharp.config`.
The installer checks the executable SHA-256:

```text
4fa94d895eae710c86709122206e0153f2a43d797f0e6f16bbd861af3e92dbd8
```

Close SDR#, then run this command from the extracted combined package:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install-sdrsharp.ps1 -SdrSharpDirectory 'C:\SDRSharp'
```

Start `SDRSharp.dotnet8.exe` and select **BirriaSDR (MT7961)** in its source
menu. The installer respects `core.pluginsDirectory`, installs beneath
`Plugins/BirriaSDR` by default and backs up previous plugin files. The plugin
uses a checked private host registration API because this host ignores
`FrontEnds.xml`. The executable and other receivers are unchanged. Next/Beta
1922 and other host builds are unsupported until separately validated.

The source has a private libusb filename to coexist with the host's runtime.
Keep the plugin's `birriasdr` subdirectory with its managed DLL. Do not copy
our USB DLL over SDR#'s existing `libusb-1.0.dll`.

Select the sample rate and analog filter while stopped. The default is
40 MSps with a nominal 40 MHz filter. The experimental 80 MHz filter requires
80 MSps. The source enables a continuous 10 Hz DC notch by default. This
removes constant offset, but cannot remove a broad pedestal or fixed RF spurs.
Native recordings and Soapy samples retain their original DC values.

To remove the source, close SDR# and run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install-sdrsharp.ps1 -SdrSharpDirectory 'C:\SDRSharp' -Uninstall
```

The uninstaller removes recorded runtime files only when their hashes still
match. Changed files are retained and reported.

## Use SoapySDR applications

The optional module targets SoapySDR 0.8 and provides CS16 and CF32 receive
streams. Set `SOAPY_SDR_PLUGIN_PATH` to the package's `soapy` directory and add
that directory to PATH. The helper can launch an installed application:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\start-soapy-app.ps1 -Executable 'C:\Program Files\PothosSDR\bin\CubicSDR.exe'
```

Use device arguments `driver=birria,index=0,experimental_mt7961=1`. Add
`experimental_tuning=1` for the wider tuning windows and
`experimental_80msps=1` for 80 MSps. Set sample rate before bandwidth while
the stream is inactive. The module rejects rate and bandwidth changes while
its acquisition worker exists, including during startup. The module is not
an ExtIO plugin.

## Build and package the integrations

Install the .NET 8 SDK and a compatible SDR# host before building the plugin.
The script extracts references from your local host and does not redistribute
the host or its SDK assemblies. The optional `-SoapyRoot` parameter points to
a SoapySDR 0.8 development installation. Omit it to build without Soapy.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-applications.ps1 -SdrSharpDirectory 'C:\SDRSharp' -SoapyRoot 'C:\Program Files\PothosSDR'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-standalone.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/package-applications.ps1
```

The combined archive contains the standalone runtime, plugin, recorder,
optional Soapy module, source archive, dependency notices and a SHA-256
manifest. Building just the standalone application does not require SDR# or
SoapySDR. A source checkout can also run
`python integrations/standalone/app.py` after building the native library and
installing NumPy.
