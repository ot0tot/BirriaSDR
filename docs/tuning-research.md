# Tuning range and oscillator mapping

The additional measurements used a USB `0e8d:7961` adapter with digital ID
`0x7961` and analog ID `0x79728a10`, libusbK and libusb 1.0.30 on Windows.
The source was an ESP32-C5 emitting two tones over the air. Source and receiver
clocks, sensitivity and antenna response were uncalibrated. These observations
do not qualify every adapter sharing this USB identity.

## Distinguish programming bounds from reception

| Profile | Accepted center frequencies | Measured scope |
| --- | --- | --- |
| Upstream/default | 2156 to 2610 MHz | The original command interval is preserved |
| Experimental low band | 1800 to 3100 MHz | Practical repeated reception near 2143 to 2612 MHz on the tested board, with drift near the edges |
| Experimental high band | 4000 to 8000 MHz | Selected positive points from 4900 to 6105 MHz, without continuous coverage or maximum limits established |

The wider windows are programming bounds, not RF specifications. The native
API requires both `BIRRIASDR_OPEN_EXPERIMENTAL_MT7961` and
`BIRRIASDR_OPEN_EXPERIMENTAL_TUNING`. The applications enable these flags for
the generic adapter. The `birriasdr_get_tuning_ranges()` function returns two
ranges. The legacy single-range getter returns their envelope, including the
unsupported gap. Requests in that gap are rejected before hardware changes.

The practical low-band endpoints came from repeated fixed-source tests.
Requests near 2142.6 MHz drifted by approximately 82 to 161 kHz. Requests at
2143 and 2612.1 MHz repeated the ordinary control's relative center error.
Requests near 2612.3 MHz drifted by approximately 79 to 165 kHz. Temperature
and unit variation remain unqualified. The source carrier stopped at 6100 MHz,
so the high-band upper receive limit could not be established.

## Program the ordinary oscillator paths

The startup sequence first establishes the original 2437 MHz anchor. The low
path uses a nominal 60 MHz reference with division by two. Its frequency word
is the rounded fixed-point divider `frequency_hz * 2 * 2^20 / 60000000`.
The high path uses the observed nominal 50 MHz coordinate without division by
two, `frequency_hz * 2^20 / 50000000`. The nominal steps are 28.61 Hz and
47.68 Hz respectively. Absolute accuracy and the physical response to the
smallest fraction bits remain unmeasured.

| Mechanism | Access / address | Known fields or current use | Limits |
| --- | --- | --- | --- |
| Receive band | RF `0x160[29]`, both ports | Clear for low, set for high | The analog filter width occupies separate bits `[31:30]` |
| Oscillator routing | RF `0xc00[5:2]`, both ports | Masked values `0x28` low and `0x38` high | A band bit alone is not a complete tuning sequence |
| Selected frequency | RF port 0 `0xca8` | Load bit 31, integer `[29:20]`, fraction `[19:0]` | Load self-clears and the remaining word is checked |
| Calibration cycles | RF port 0 `0xcac` | Low `floor(hz / 125000) - 2`, high `floor(hz * 24 / 5000000) - 2` | Arithmetic and completion do not establish PLL lock |
| Calibration range code | RF port 0 `0xcb8[19:4]` | Low `0x14 * 0x1010`, high code table in source | High-band response remains partially characterized |
| Calibration handshake | RF port 0 `0xcc4`, `0xcc8`, `0xce0`, `0xd20` | Start, poll `0xd7c[21]`, latch `0xd78[2:0]` into `0xc0c[24:22]`, then stop | Completion is not a qualified lock detector |
| Temporary routing | RF port 0 `0x240[31:28]` | Saved around calibration and restored | Live retuning disturbs samples and phase |

The implementation saves band fields before its first change, checks masked
readback and restores them during cleanup. A failed live retune attempts the
previous center before capture is cancelled. Only a successful setter commits
the requested center. The standalone app restarts acquisition on retuning to
avoid giving old queued samples the new frequency label.

Implementation: [mt7921_tune.c](../src/chips/mt7921_tune.c) and
[mt7921_rf.c](../src/chips/mt7921_rf.c).

## Understand the measured 5/6 LO mapping

The alternate low-band selectors `0x2c` and `0x3c` under RF `0xc00` mask
`0x3c` on both ports produced a 5/6 mapping when applied before calibration.
Keep the ordinary low frequency-word calculation, change the selector before
calibration and interpret physical frequency as approximately 5/6 of that
programmed coordinate. Preserve and restore all changed fields.

The independent test held the source at 2150 MHz and programmed 2574, 2580
and 2586 MHz. Its two markers moved from about +6.25/+8.75 MHz through
+1.25/+3.75 MHz to −3.75/−1.25 MHz. The inferred center slope was about
0.833 per programmed hertz. Further positive pairs were observed at physical
2150, 2200, 2300 and 2400 MHz with programmed coordinates 2580, 2640, 2760
and 2880 MHz using selector `0x2c`.

This confirms a frequency mapping on this board, not an internal divider
schematic or an additional receive range. It demonstrated no extension below
the ordinary practical low-band interval. The public applications therefore
retain the ordinary low/high mappings and do not expose a 5/6 switch. The
mapping experiment is documented so it can be reproduced in an instrumented
register test, without carrying the private bench harness into the release.

## Reproduce reception and avoid misleading tests

Use a stable source with known offsets, for example two tones at +1.25 and
+3.75 MHz. Capture source-off and source-on windows with fixed manual gain.
First verify the source at an ordinary center. Hold its carrier fixed while
stepping the receiver, then check that both markers move by the expected
amount. Repeat near an apparent edge and inspect drift. A positive detection
alone does not prove accurate tuning or continuous coverage.

The public recorder can exercise the same high-band path as the applications:

```powershell
.\build-windows\Release\birria_rx.exe --experimental-mt7961 --experimental-tuning --frequency 5800000000 high.cs16 8000000 manual
```

The following unsuccessful approaches should not be treated as capabilities:

- The diagnostic RF port 0 `0xd74` rail values contradicted reception at
  2156, 2610 and 6100 MHz. The experimental profile does not use them as a
  lock veto. The upstream default behavior remains unchanged.
- The alternate selectors applied after calibration did not recover the
  expected 5/6 pair. Most other tested selector encodings were negative.
- The high-band coordinate 4333.333333 MHz did not receive a 5200 MHz source,
  although direct high-band programming at 5200 MHz did. That 6/5 assumption
  was not established.
- The high coordinate 5200 MHz with a 2600 MHz source did not establish a
  useful half-frequency path in the tested selectors and sequences.
- The LimeSDR host stream ran, but its expected markers were not identified.
  Those runs provide no tuning-limit evidence. A missing marker can also be
  a source or RF-path failure.
