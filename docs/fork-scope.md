# Receive fork scope

This branch maintains BirriaSDR as a receive-only fork for the generic MT7961
USB adapter and its Windows applications. Its upstream base is commit
`9a0a00321ade8b66e25e32f9e6e442c1faef88da`, fetched on October 17, 2026.
The original library, licenses, device program and COMFAST profile are retained.

## Changes from upstream

| Area | Changes | Compatibility boundary |
| --- | --- | --- |
| Platform | Windows synchronization, timing, USB interface discovery and libusbK raw reads | The original POSIX implementation remains selected on other platforms |
| Adapter | Explicit `0e8d:7961` board opt-in and identity checks | Other board identities remain restricted |
| Tuning | Separate low/high programming windows, high-band sequence and range enumeration | The original default profile remains unchanged, wider coverage is experimental |
| Capture | Experimental 80 MSps clock and 80 MHz filter, restored on cleanup | The default remains 40 MSps and nominal 40 MHz filtering |
| Transport | Pooled buffer ownership, larger 80 MSps reads and aggregation, reduced parser copying | The firmware and strict continuity checks remain unchanged |
| Applications | Standalone spectrum receiver, SDR# source, optional SoapySDR module and packaging | The SDR# registration adapter supports one checked host build |
| Documentation | Setup, measured ranges, 5/6 LO mapping, gain fields, bandwidth and failure limits | Bench automation and raw measurement archives are excluded |

The branch contains no transmit experiments, C5 firmware, ROM dumps, raw RF
captures, copied development trees or machine-specific build paths. Focused
tests remain because they verify discovery, register preservation, rollback,
continuity and application behavior. Normal builds use the unchanged embedded
original program and require no vendor firmware or device flashing.

The comparison touches 75 files. The core `src` and `include` tree grows from
3,518 to 4,037 lines, about 15 percent. The applications, packaging, tests and
documentation account for most of the added material. The complete tracked
source tree is approximately 0.89 MB, without build outputs or measurement
archives. These figures describe this initial release branch against the base
above, rather than the size of a download containing Python and NumPy.

## Distribution strategy

The combined application and experimental RF changes are broad enough to
maintain as a separate fork. Windows dependency packaging, a private SDR# host
adapter and the browser UI introduce maintenance obligations beyond adding one
USB identity. They also need release checks when dependencies or host builds
change. This branch has been tested on the attached Windows host and adapter.
The original upstream Linux hardware results do not requalify these additions
on Linux or macOS.

A later upstream contribution could isolate portable Windows support and
descriptor discovery first, followed by bounded transport improvements and
separately reviewed tuning options. Such contributions need upstream agreement
on experimental board policy and additional platform testing. Publishing this
whole fork as a small adapter-support PR would understate its scope.

The local release branch starts directly from upstream rather than inheriting
the development archive commits. Historical research remains on separate local
branches. A public release should push this release branch, without publishing
those archival branches by default.

## Release checks

Build the native library and run CTest, then run the standalone host tests:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-windows.ps1
python -m unittest discover -s tests -p test_standalone.py -v
```

Build the plugin and optional module using the
[integration instructions](applications.md#build-and-package-the-integrations).
The installer fixture checks installation without `FrontEnds.xml`, repeated
installation, backups, removal ownership and unsupported-host rejection:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/test_installer.ps1 -SdrSharpDirectory 'C:\SDRSharp' -PackageDirectory "$PWD\build-applications\sdrsharp"
```

The managed smoke project can run `--dsp` and `--panel output.png` without
opening USB. Other smoke modes open physical hardware and require exclusive
access. The portable acceptance runner also opens hardware unless `--demo`
is supplied:

```powershell
python tools/validate_standalone_package.py --demo --out build-package-demo
```

The [transport report](usb-streaming.md) preserves the relevant long-run
results and unsuccessful profiles in concise form. A short release check does
not replace those measurements or establish calibrated RF performance.

The clean-branch checks on October 17, 2026 passed twelve native CTest cases,
six standalone host tests, managed scaling/DC tests and the installer fixture.
The browser check passed tuning, gain, snapshots, narrow layout and hidden
canvas handling without script errors. The fresh portable application delivered
2,360,612,864 complex samples in its 30-second 80 MSps check at 5800 MHz with
a 262,144-point FFT. It ended in firmware state 3 with no DMA or reset error.
The queue peak was one of 512 slots and the observed source backlog was
4,472 samples. This short check did not sweep RF bandwidth.

The actual SDR# frontend passed 40 MSps, 80 MSps with the narrower filter and
80 MSps with the wider filter, including live retunes. Its test loaded the
host's existing USB runtime before our private runtime. The Soapy module
passed CS16 and CF32 in those three profiles, including partial reads, live
gain and tuning. A final native hardware test completed finite captures,
gain-mode changes and asynchronous cancellation cleanly. Receiver processes
were closed and cooperative hardware ownership was released.
