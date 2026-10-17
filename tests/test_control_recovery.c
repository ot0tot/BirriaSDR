/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/*
 * Explicit receive-only hardware regression for failed live configuration.
 *
 * Wrap the host register transport and fail selected writes during one gain or
 * synthesizer transaction. All other accesses reach the real dongle. A second
 * injected failure can reject rollback as well. This models a host I/O failure,
 * not a physical disconnection or an independent RF calibration measurement.
 *
 * Run controls on another application thread so the sample callback keeps
 * draining data. Automatic recovery must notify one gap, restart without an
 * application read loop, and retain the preceding committed frequency/gain.
 * No vendor firmware, kernel driver, flash write or power cycle is used.
 */
#include "chips/mt7921.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

struct fault {
    struct bs_io original;
    atomic_uint remaining, failures;
    uint32_t command;
    bool tune;
};
struct run {
    birriasdr_dev_t *dev;
    struct fault *fault;
    atomic_uint_fast64_t samples;
    atomic_bool done;
    uint64_t resume_until;
    unsigned gaps, injections;
    int setter, callback_error;
    struct birriasdr_recovery_event gap;
};

/* Reads still go through the library's userland register transport. */
static int read_word(void *context, enum bs_space space, uint32_t address, uint32_t *value)
{
    struct fault *fault = context;
    return fault->original.read32(fault->original.context, space, address, value);
}

/* Target calibration START, or the last PHY gain write, under the library's
 * control mutex. Limit injection count so startup after recovery can succeed.
 */
static int write_word(void *context, enum bs_space space, uint32_t address, uint32_t value)
{
    struct fault *fault = context;
    if (space == BS_BRIDGE && address == 0x7c004010) fault->command = value;
    bool target = fault->tune ?
        space == BS_BRIDGE && address == 0x7c004014 && (fault->command & 0x1fff) == 0xce0 &&
        value == 0x42956473 : space == BS_MMIO && address == 0x830000c4;
    if (target && atomic_load(&fault->remaining)) {
        atomic_fetch_sub(&fault->remaining, 1);
        atomic_fetch_add(&fault->failures, 1);
        return BIRRIASDR_ERROR_IO;
    }
    return fault->original.write32(fault->original.context, space, address, value);
}

/* Count valid delivered data through one failed acquisition and its restart. */
static void receive(const int16_t *iq, uint32_t count, uint64_t first, void *context)
{
    struct run *run = context;
    if (!iq || first != atomic_load(&run->samples)) run->callback_error = BIRRIASDR_ERROR_PROTOCOL;
    atomic_fetch_add(&run->samples, count);
    if (run->callback_error || (run->gaps && first + count >= run->resume_until) ||
        first > UINT64_C(40000000) * 5)
        birriasdr_cancel_async(run->dev);
}

/* Record the failed control's diagnostics before the next boot clears them. */
static void gap(const struct birriasdr_recovery_event *event, void *context)
{
    struct run *run = context;
    run->gap = *event;
    ++run->gaps;
    if (run->gaps != 1 || event->next_sample != atomic_load(&run->samples))
        run->callback_error = BIRRIASDR_ERROR_PROTOCOL;
    run->resume_until = event->next_sample + 65536;
    if (run->callback_error) birriasdr_cancel_async(run->dev);
}

/* Arm only after streaming begins; the setter owns the actual RF transaction. */
static void *change_configuration(void *context)
{
    struct run *run = context;
    while (!atomic_load(&run->done) && atomic_load(&run->samples) < 1048576)
        bs_sleep_us(500);
    if (!atomic_load(&run->done)) {
        struct birriasdr_gain request = {1, 3, 2};
        atomic_store(&run->fault->remaining, run->injections);
        run->setter = run->fault->tune ? birriasdr_set_center_freq(run->dev, UINT64_C(2442000000)) :
                                       birriasdr_set_gain_codes(run->dev, &request);
    }
    return NULL;
}

/* Keep one handle through all trials; each read must recover internally. */
static int trial(birriasdr_dev_t *dev, bool tune, unsigned injections)
{
    struct fault fault = {.original = dev->io, .tune = tune};
    struct run run = {.dev = dev, .fault = &fault, .injections = injections};
    struct birriasdr_gain actual;
    struct birriasdr_stream_stats stats;
    pthread_t worker;
    int manual, result;
    dev->io = (struct bs_io){&fault, read_word, write_word};
    result = birriasdr_set_recovery(dev, 2, gap, &run);
    if (!result && pthread_create(&worker, NULL, change_configuration, &run)) result = BIRRIASDR_ERROR_IO;
    if (!result) {
        result = birriasdr_read_async(dev, receive, &run, 0, 0);
        atomic_store(&run.done, true);
        pthread_join(worker, NULL);
    }
    birriasdr_get_stream_stats(dev, &stats);
    birriasdr_get_gain_codes(dev, &actual, &manual);
    dev->io = fault.original;
    int expected = injections == 1 ? BIRRIASDR_ERROR_IO : BIRRIASDR_ERROR_STATE;
    if (!result && (atomic_load(&fault.failures) != injections ||
        run.setter != expected || run.gaps != 1 || run.gap.error != expected ||
        run.gap.previous.stage != (tune ? BIRRIASDR_STAGE_RF : BIRRIASDR_STAGE_GAIN) ||
        run.gap.previous.reset_error || run.callback_error || stats.firmware_state != 3 ||
        stats.reset_error || stats.dma_errors || stats.samples < run.resume_until ||
        stats.samples != atomic_load(&run.samples) || !manual || actual.lna != 2 ||
        actual.lpf != 8 || actual.digital != 0 || birriasdr_get_center_freq(dev) != UINT64_C(2437000000)))
        result = BIRRIASDR_ERROR_STATE;
    printf("{\"event\":\"configuration_recovery\",\"tune\":%s,\"injected\":%u,"
           "\"setter\":%d,\"gap_error\":%d,\"gap_stage\":%u,\"gaps\":%u,"
           "\"samples\":%" PRIu64 ",\"state\":%u,\"result\":%d}\n",
           tune ? "true" : "false", atomic_load(&fault.failures), run.setter, run.gap.error,
           (unsigned)run.gap.previous.stage, run.gaps, stats.samples, stats.firmware_state, result);
    return result;
}

int main(int argc, char **argv)
{
    birriasdr_dev_t *dev = NULL;
    uint32_t open_flags = 0;
    struct birriasdr_gain gain = {2, 8, 0};
    if (argc == 3 && !strcmp(argv[2], "--experimental-mt7961"))
        open_flags = BIRRIASDR_OPEN_EXPERIMENTAL_MT7961;
    if ((argc != 2 && !(argc == 3 && open_flags)) || strcmp(argv[1], "--run")) {
        fprintf(stderr, "Usage: %s --run [--experimental-mt7961] (injects control failures on a physical dongle)\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    int result = birriasdr_open_ex(&dev, 0, open_flags);
    if (!result) result = birriasdr_set_tuner_gain_mode(dev, 1);
    if (!result) result = birriasdr_set_gain_codes(dev, &gain);
    for (unsigned i = 0; i < 4 && !result; ++i)
        result = trial(dev, i >= 2, i % 2 + 1);
    int closed = dev ? birriasdr_close(dev) : 0;
    printf("{\"event\":\"end\",\"result\":%d,\"close\":%d}\n", result, closed);
    return result || closed ? 1 : 0;
}
