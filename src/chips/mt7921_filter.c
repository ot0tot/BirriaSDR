/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/*
 * MT7961/MT7972 matched receive bandwidth
 *
 * The analog filter and the digital PHY bandwidth are independent controls.
 * RF 0x160[31:30] selects the analog profile in each physical SPI bank;
 * 0x83081000[27:24] and 0x83091000[27:24] select the digital bandwidth class.
 * The original channel-6 bootstrap leaves both at code 0. Merely widening the
 * analog filter retains the digital rolloff: noise is about 22 dB down at
 * +/-18 MHz with analog code 1 and digital code 0.
 *
 * Set both fields to code 1 after the established RF/gain bootstrap, while
 * capture is stopped and the original MCU program waits for GO. This is the
 * matched 40 MHz profile. The capture tap and divider remain unchanged at
 * nominal 40 MSps complex; no host equalizer, averaging or format conversion
 * is introduced. The paired configuration reduced the measured noise rolloff
 * to about 2.8 dB at +/-18 MHz on the tested board.
 *
 * This is a nominal channel profile, not a brick-wall anti-alias filter. The
 * receive-only Wi-Fi comparison measured roughly 19-20 dB suppression at
 * +/-30 MHz and still decoded some aliased headers from strong signals there.
 * An explicit experimental 80 MHz profile selects code 2 in both fields and
 * requires 80 MSps. The rate alone keeps code 1. These profiles do not establish
 * a flat passband or stopband guarantee; see docs/bandwidth-validation.md.
 *
 * Save all four baseline fields before the first write. Preserve unrelated
 * bits, verify each application, and restore every field on partial failure.
 * Analog state can survive WFSYS reset, so normal reset restores this baseline
 * too. A failed restoration keeps the saved state for retry. Every automatic
 * restart follows the same bootstrap and reapplies this profile before GO.
 */
#include "mt7921.h"
#include <string.h>

static const uint32_t filter_addresses[] = {0x160, 0x160, 0x83081000, 0x83091000};

/* The first two fields use physical RF banks; the remaining two use MMIO. */
static int filter_read(birriasdr_dev_t *dev, unsigned field, uint32_t *value)
{
    if (field < 2)
        return mt_rf_read(dev, field, filter_addresses[field], value);
    return bs_read32(dev, BS_MMIO, filter_addresses[field], value);
}

/* Change only the bandwidth field, retaining band selection and other modes.
 * Masked verification accepts independent changes to unrelated register bits.
 */
static int filter_write(birriasdr_dev_t *dev, unsigned field, uint32_t value)
{
    uint32_t mask = field < 2 ? UINT32_C(0xc0000000) : UINT32_C(0x0f000000);
    uint32_t actual;
    if (field < 2)
        BS_TRY(mt_rf_update(dev, field, filter_addresses[field], mask, value & mask));
    else
        BS_TRY(bs_update32(dev, BS_MMIO, filter_addresses[field], mask, value & mask));
    BS_TRY(filter_read(dev, field, &actual));
    return ((actual ^ value) & mask) ? BIRRIASDR_ERROR_STATE : 0;
}

/* Restore digital class first, then analog width, before the subsystem reset.
 * Attempt every field even if one fails; retain the baseline until all pass.
 */
int mt_restore_filter(birriasdr_dev_t *dev)
{
    static const unsigned order[] = {2, 3, 0, 1};
    int first_error = 0;
    if (!dev->mt7921.filter_saved)
        return 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned field = order[i];
        int result = filter_write(dev, field, dev->mt7921.filter_baseline[field]);
        if (result < 0 && !first_error)
            first_error = result;
    }
    if (!first_error)
        dev->mt7921.filter_saved = false;
    return first_error;
}

/* Apply the selected profile once per preparation; this is not a live control.
 * The prepare owner handles any remaining cleanup and whole-startup retry.
 */
int mt_configure_filter(birriasdr_dev_t *dev)
{
    uint32_t before[4];
    unsigned code=dev->bandwidth==80000000 ? 2u : 1u;
    if (dev->rate != 40000000 && !(dev->rate==80000000 && (dev->open_flags&BIRRIASDR_OPEN_EXPERIMENTAL_80MSPS)))
        return BIRRIASDR_ERROR_UNSUPPORTED;
    if (code==2 && (dev->rate!=80000000 || !(dev->open_flags&BIRRIASDR_OPEN_EXPERIMENTAL_80MSPS)))
        return BIRRIASDR_ERROR_UNSUPPORTED;
    if (dev->mt7921.filter_saved)
        return BIRRIASDR_ERROR_STATE;
    for (unsigned i = 0; i < 4; ++i)
        BS_TRY(filter_read(dev, i, &before[i]));
    memcpy(dev->mt7921.filter_baseline, before, sizeof(before));
    dev->mt7921.filter_saved = true;
    for (unsigned i = 0; i < 4; ++i) {
        int result = filter_write(dev, i, code << (i < 2 ? 30 : 24));
        if (result < 0)
            return mt_restore_filter(dev) < 0 ? BIRRIASDR_ERROR_STATE : result;
    }
    bs_sleep_us(20000);
    return 0;
}
