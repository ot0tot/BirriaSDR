# Receive transport and measured limits

The library keeps the upstream original 1,120-byte device program and its
strict nonce, sequence, framing and sample checks. The changes optimize host
buffer ownership, read sizes and device event aggregation. They do not hide
capture loss or alter raw sample precision.

## Follow the receive path

The capture engine writes a 65,536-complex-sample circular SRAM ring. The
device program submits serial DMA descriptors into USB. Each event contains
44 header bytes, 8,192 sample bytes and four trailer bytes. Its 2,048 complex
samples retain signed 14-bit values in 16-bit slots. Throughput including event
framing is about 160.94 MB/s at 40 MSps and 321.88 MB/s at 80 MSps, before USB
protocol overhead.

| Host profile | Submitted reads | Queue slots | Device aggregation size code | Approximate buffer pool |
| --- | --- | --- | --- | --- |
| Default 40 MSps | 64 × 64 KiB | 512 | 32 KiB | 36 MiB |
| Experimental 80 MSps | 64 × 256 KiB | 512 | 232 KiB | 144 MiB |

The event thread swaps preallocated buffers into the queue and submits a
spare. The consumer copies after releasing the queue lock. The complete pool
is allocated and touched before GO. The parser handles arbitrary completion
boundaries and copies only fragmented events into its assembly buffer.
Windows raw I/O is enabled after ROM setup and disabled after transfer drain.
The tested path uses libusbK and the pinned libusb 1.0.30 runtime.

Implementation: [usb_stream.c](../src/usb_stream.c),
[mt7921_boot.c](../src/chips/mt7921_boot.c) and
[mt7921_frame.c](../src/chips/mt7921_frame.c).

## Separate display skips from capture loss

The standalone callback remains in native code. It drains validated samples
and selects complete FFT windows into a bounded mailbox. A busy display skips
a window without blocking acquisition. The largest 262,144-point FFT is
limited to ten display updates per second. Smaller transforms allow twenty.
The display does not analyze every received sample and can miss short bursts.

A host queue overrun means completed USB buffers could not be retained.
A device ring overrun means the device producer overtook the DMA reader.
The unchanged firmware guard is 61,440 samples. A large host queue cannot
prevent a device ring overrun. Either error stops acquisition with continuity
lost. The application saves settings and available transport diagnostics.

## Device overrun with large FFTs

The final pre-cleanup portable application ran 602.953 seconds at 2437 MHz,
80 MSps, nominal 80 MHz filter, released gain and a live browser displaying
262,144-point FFTs. It delivered 48,191,940,608 complex samples and 5,931 display
frames. The firmware ended in state 3, DMA and reset errors were zero and the
host queue peaked at 18 of 512 slots. The largest observed source backlog
was 5,220 samples. Completion size reached 238,960 bytes, below the 256 KiB
read size. These are results from one Windows host and adapter, not a latency
guarantee.

The final 40 MSps run lasted 60.657 seconds while FFT size changed from 4,096
through 16,384 and 65,536 to 262,144. It delivered 2,405,494,784 samples without
a stream error. Its backlog reached 59,772 samples, leaving limited margin
below the guard. Heavy host scheduling stalls can still cause a failure.

Earlier 64 KiB reads with 56 KiB aggregation reproduced a device overrun at
the largest FFT while the host queue used only four of 2,048 slots. A
128 KiB / 120 KiB profile passed a short test but failed after about 72 seconds
in the packaged application. The release uses the larger 256 KiB / 232 KiB
profile at 80 MSps. Python callbacks on every sample block also caused host
overruns, which motivated the native display bridge.

## Bit depth and capture word width

The tested raw components had two zero low bits in more than five million
observed values. This is occupied-bit evidence, not effective ADC resolution.
The library retains the full signed 14-bit format and accepts all valid values.
Tests of four capture packers did not establish a balanced 8-bit I/Q mode.
The upstream wider-clock suggestions supported the selected 80 MSps path,
but did not qualify a reduced-bandwidth USB format at 160 MSps.

The public API therefore rejects 160 MSps. Host decimation can reduce
application processing or file size after USB transfer, but cannot lower USB
traffic. Device-side compaction and a validated decimator would require new
firmware and a new sample/protocol qualification.

## Avoid unsuccessful recovery and tuning changes

Keep one SRAM-backed DMA descriptor outstanding on the device. Earlier
concurrent descriptors produced corruption even though host reads can safely
remain queued. Keep the pinned runtime, since an older Windows runtime
crashed. Avoid the libusbK maximum-transfer-size query in this runtime, which
hit an unavailable backend function during testing. The qualified sizes above
are selected explicitly.

Some failed bulk transports required physical reconnects. Endpoint stall
clearing and software reset did not recover every fault. Save diagnostics,
close all receiver applications and reconnect before retrying. No receiver
application silently retries a broken stream as if continuity were intact.
