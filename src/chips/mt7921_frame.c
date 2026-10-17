/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/*
 * Original MT7961 firmware event framing and raw I/Q conversion
 *
 * This is the protocol emitted by firmware/iq_stream.S, not a general Wi-Fi
 * packet decoder. USB transfer boundaries can split or aggregate events; the
 * framer keeps at most one bounded incomplete event and consumes arbitrary
 * input fragments. Do not assume one libusb completion equals one DMA packet.
 *
 * An I/Q event is 8,240 bytes. Multi-byte wire fields are little-endian:
 *   Offset  Size  Meaning
 *        0     4  RXD0: class 7 in [31:27], length 8,236 in [15:0].
 *       24     4  MCU event word: 0x38000014 + payload size (8,192).
 *       28     1  Event ID 0xed.
 *       32     1  Extended event ID 0xf5.
 *       36     4  Packet sequence, starting at zero for this capture.
 *       40     4  Fresh boot nonce.
 *       44  8192  2,048 raw complex samples, Q16 followed by I16.
 *     8236     4  USB trailer, outside both the RXD0 length and I/Q payload.
 * The remaining header fields are not interpreted here. The trailer may be a
 * neighboring SRAM word: ignore its contents and never deliver it as a sample.
 * One 40-byte ROM start ACK (event ID 1, status zero) is accepted; duplicate
 * ACKs are rejected. That event is left over from the boot handoff.
 *
 * Check event identity, nonce and sequence before delivering each payload.
 * Sequence comparison wraps at the 32-bit wire width while the host maintains
 * a 64-bit packet total. Reject gaps/repeats instead of scanning for a valid
 * next header: resynchronization would conceal a discontinuous capture.
 * bs_framer_finish() also rejects a partial event or a packet-total mismatch.
 * The stream layer separately checks firmware terminal state and DMA errors;
 * receiving a valid prefix alone does not establish a successful whole read.
 *
 * Each component is 14-bit two's complement in a 16-bit slot whose upper two
 * bits must be zero. Validate the entire payload before converting any samples.
 * Sign-extend bit 13 and swap Q,I to the API's native-endian int16_t I,Q order,
 * preserving the range -8192..8191. There is no normalization, DC correction,
 * DDC, averaging or decimation here. A new capture format must change this
 * decoder explicitly rather than silently reinterpreting the existing bytes.
 * tests/test_core.c exercises fragmentation, trailers and integrity failures.
 */
#include "internal.h"
#include <string.h>

/* Validate framing before releasing samples. Each event contains a monotonically
 * increasing sequence and fresh boot nonce; both detect gaps, repeats and stale
 * runs. The final USB trailer is outside the I/Q payload and may contain SRAM
 * data, so its value is deliberately not interpreted as another sample.
 */
static int consume_frame(struct bs_framer *framer, const uint8_t *frame, size_t length)
{
    if (length == 40) {
        if (frame[28] != 1 || frame[32] || framer->acknowledgements++)
            return BIRRIASDR_ERROR_PROTOCOL;
        return 0;
    }
    if (frame[28] != 0xed || frame[32] != 0xf5 ||
        bs_get_le32(frame + 24) != 0x38000014u + BS_PAYLOAD_BYTES ||
        bs_get_le32(frame + 36) != (uint32_t)framer->packets ||
        bs_get_le32(frame + 40) != framer->nonce)
        return BIRRIASDR_ERROR_PROTOCOL;
    BS_TRY(framer->payload(framer->context, frame + 44));
    ++framer->packets;
    return 0;
}

/* USB transfer boundaries are unrelated to DMA event boundaries. Assemble at
 * most one bounded frame, supporting every split point and aggregated events.
 * Invalid headers fail immediately instead of scanning ahead and hiding loss.
 */
int bs_framer_feed(struct bs_framer *framer, const uint8_t *data, size_t length)
{
    while (length) {
        size_t target = 4, copy;
        if (!framer->used && length >= 4) {
            uint32_t word = bs_get_le32(data), size = word & 0xffff;
            if (word >> 27 != 7 || (size != 36 && size != BS_FRAME_BYTES - 4))
                return BIRRIASDR_ERROR_PROTOCOL;
            if (length >= size + 4u) {
                BS_TRY(consume_frame(framer, data, size + 4u));
                data += size + 4u;
                length -= size + 4u;
                continue;
            }
        }
        if (framer->used >= 4) {
            uint32_t word = bs_get_le32(framer->pending);
            uint32_t size = word & 0xffff;
            if (word >> 27 != 7 || (size != 36 && size != BS_FRAME_BYTES - 4))
                return BIRRIASDR_ERROR_PROTOCOL;
            target = size + 4;
        }
        copy = target - framer->used;
        if (copy > length)
            copy = length;
        memcpy(framer->pending + framer->used, data, copy);
        framer->used += copy;
        data += copy;
        length -= copy;
        if (framer->used == target && target != 4) {
            BS_TRY(consume_frame(framer, framer->pending, target));
            framer->used = 0;
        }
    }
    return 0;
}

/* A successful terminal mailbox is insufficient if USB ended mid-frame or lost
 * a whole packet. Match the independent firmware packet total as well.
 */
int bs_framer_finish(const struct bs_framer *framer, uint64_t expected)
{
    return framer->used || framer->packets != expected ? BIRRIASDR_ERROR_PROTOCOL : 0;
}

/* Raw SRAM is little-endian Q then I, 14-bit two's-complement in 16-bit slots.
 * Sign-extend and reorder to conventional native-endian I,Q without rescaling.
 * Validate the entire packet before making any part of it visible to a caller.
 */
int bs_decode_iq(const uint8_t *raw, int16_t *iq, size_t samples)
{
    for (size_t n = 0; n < samples; ++n)
        if (bs_get_le32(raw + n * 4) & 0xc000c000u)
            return BIRRIASDR_ERROR_PROTOCOL;
    for (size_t n = 0; n < samples; ++n) {
        uint32_t word = bs_get_le32(raw + n * 4);
        iq[n * 2] = (int16_t)((int32_t)(((word >> 16) & 0x3fff) ^ 0x2000) - 0x2000);
        iq[n * 2 + 1] = (int16_t)((int32_t)((word & 0x3fff) ^ 0x2000) - 0x2000);
    }
    return 0;
}
