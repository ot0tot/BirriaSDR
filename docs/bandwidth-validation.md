# Receive bandwidth and gain limits

The default profile uses 40 MSps and a nominal 40 MHz filter. The experimental
80 MSps rate and 80 MHz filter are separate controls. Selecting the rate alone
retains the narrower filter. Selecting the wider filter requires 80 MSps.
The tested USB `0e8d:7961` board has not demonstrated a flat 80 MHz usable
passband.

## Select the filter and clock together

| Mechanism | Access / address | Known fields or current use | Limits |
| --- | --- | --- | --- |
| Analog width | RF `0x160[31:30]`, both ports | Code 1 for nominal 40 MHz, code 2 for experimental 80 MHz | Independent of filter-stage gain |
| Digital bandwidth | MMIO `0x83081000[27:24]` and `0x83091000[27:24]` | Matching class 1 or 2 | Widening only the analog field retains digital rolloff |
| Capture clock | MMIO `0x830ad448` | Divider `[19:18]` is 3 at 40 MSps and 2 at 80 MSps | The backend owns mask `0x028c0000` during the rate change |
| Additional clock field | MMIO `0x81026004` | Value `0x28` under mask `0xfc` at 80 MSps | Required with the tested higher-rate profile |

The backend saves, verifies and restores its modified fields. Rate and filter
changes require an inactive capture. The [backend reference](mt7921.md#filtering)
describes the startup and rollback ordering.

## Interpret the RF measurements

The sweep used an ESP32-C5 source with fixed carrier and tones at +1.25 and
+3.75 MHz. The receiver moved across those tones with manual gain 1 / 4 / 0.
The nearby over-the-air chain was uncalibrated. The levels below compare each
tone against its first center reference on the tested board.

| Fixed source carrier | Response at −38.75 MHz | Response at +38.75 MHz | Center-repeat change |
| --- | --- | --- | --- |
| 2437 MHz | −9.4 dB | −9.3 dB | −0.9 and +0.2 dB |
| 5200 MHz | +1.2 dB | +1.9 dB | +4.6 and +5.3 dB |
| 6050 MHz | −2.7 dB | −3.6 dB | +1.3 and +1.0 dB |

The large center-repeat change at 5200 MHz prevents a reliable flatness claim.
At 2437 MHz, sampled points within 3 dB of center extended from −18.75 to
+21.25 MHz. These sparse points are not exact passband endpoints. The wider
filter still improved the response at ±38.75 MHz from about −38 dB with the
narrower filter to about −9 dB.

The wider profile also admitted aliases. A source at +40.75 MHz folded to
−39.25 MHz and a source at +45.75 MHz folded to −34.25 MHz. The first alias
was only about 10 dB below the center response in the low band. A host filter
cannot separate an alias already folded onto a wanted signal. Use an external
filter when strong out-of-span signals matter.

To repeat a relative response test, hold the source carrier, waveform and
manual gain fixed. Alternate each offset capture with a center reference.
Check clipping and source-off contrast. Preserve physical and folded offset
coordinates separately. A sample-rate setting or a successful USB stream is
not a usable-bandwidth measurement.

## Use the mapped gain controls

The manual controls combine LNA codes 0 to 3, filter gain 0 to 8 and digital
gain 0 to 12. These are raw codes rather than calibrated dB. The
[gain register table](mt7921.md#gain-controls) gives the implemented fields,
ownership masks and restore order. Filter gain does not select filter width.

The tested low-band source tones increased with LNA code. Filter gain was
not a linear dB progression. Digital steps from zero to six to twelve gave
about 6 dB per six steps locally. At 5800, 5900 and 6000 MHz, digital code 12
increased the controlled tones by about 12 dB relative to zero. These results
do not establish input sensitivity or a calibrated gain scale.

Releasing manual overrides restores the saved unforced baseline. Applied
analog indices at RF port 1 `0x268[23:21]` for LNA and `[20:17]` for filter
gain changed with source level at 2437 MHz. This demonstrates adaptive gain
there, without qualifying a global AGC controller. Target, attack, decay and
complete loop freeze remain unsupported. The public AGC setter returns
unsupported. Forcing the eight coarse states at MMIO `0x830000d0[26:24]`
with bit 27 set did not freeze every gain loop and sometimes clipped severely.

## Recognize high-band artifacts

The native captures contained lines near 5879.967 and 5919.967 MHz that stayed
at those RF frequencies when the receiver retuned. Removing the antennas
reduced the pair by roughly 10 dB at fixed manual gain. This supports RF-path
coupling rather than a sample-rate/4 formatting error, without identifying
the emitter or every internal coupling path.

The raw decoder preserves signed 14-bit Q/I slots and returns I/Q pairs on
the 8192 scale. The SDR# notch and standalone mean subtraction remove narrow
DC offsets only. They cannot calibrate sensitivity, remove a broad center
pedestal or suppress fixed RF lines. High-band gain and I/Q imbalance remain
uncalibrated.

Implementation: [mt7921_filter.c](../src/chips/mt7921_filter.c),
[mt7921_capture.c](../src/chips/mt7921_capture.c),
[mt7921_gain.c](../src/chips/mt7921_gain.c) and
[IqConverter.cs](../integrations/sdrsharp/IqConverter.cs).
