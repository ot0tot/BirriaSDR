/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/* Exercise the real public read engine and packet decoder with a simulated
 * transport/backend. Faults deliberately cross lifecycle stages, including a
 * terminal firmware fault whose submitted packets can never all reach USB.
 * This tests ownership/error reporting, not analog or physical USB behavior.
 */
#include "usb_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(t) do { if (!(t)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #t); exit(1); } } while (0)

enum fault { NONE, CREATE, PREPARE, SUBMIT, START, STOP, STATUS, RESET,
             BAD_FRAME, SOURCE_OVERRUN, DMA_ERROR, GAIN_ERROR, TUNE_ERROR, CANCEL_PREPARE,
             TUNE_STOP_ERROR, REMOVED, RETAINED_PATCH, RF_STATE };
struct model {
    enum fault fault;
    unsigned resets, starts, stops, created, destroyed;
    uint32_t limit, generated, terminal;
    bool started, stopped, tune_called;
    unsigned preparations;
    bool recovery_test, persistent;
};
struct bs_reader { birriasdr_dev_t *dev; };
struct observation { birriasdr_dev_t *dev; uint64_t samples; unsigned callbacks; };

/* The test handle is already open; reaching discovery would be a test bug. */
int bs_usb_discover(uint32_t index, struct birriasdr_device_info *info, birriasdr_dev_t *dev)
{
    (void)index; (void)info; (void)dev;
    abort();
}
void bs_usb_close(birriasdr_dev_t *dev) { (void)dev; abort(); }

/* Retain a borrowed test device until destruction, including failed startup. */
int bs_reader_create(birriasdr_dev_t *dev, unsigned depth, struct bs_reader **out)
{
    struct model *model = dev->io.context;
    CHECK(depth >= 4 && depth <= 256);
    *out = NULL;
    if (model->fault == CREATE) return BIRRIASDR_ERROR_NO_MEMORY;
    *out = calloc(1, sizeof(**out));
    CHECK(*out);
    (*out)->dev = dev;
    ++model->created;
    return 0;
}
int bs_reader_start(struct bs_reader *reader)
{
    struct model *model = reader->dev->io.context;
    return model->fault == SUBMIT ? BIRRIASDR_ERROR_IO : 0;
}
void bs_reader_destroy(struct bs_reader *reader)
{
    if (reader) {
        struct model *model = reader->dev->io.context;
        ++model->destroyed;
        free(reader);
    }
}

/* Generate independently specified wire events. Faulted firmware can advertise
 * more submitted descriptors than it delivered; the host must not await them
 * indefinitely or report a generic timeout instead of the known source fault.
 */
int bs_reader_take(struct bs_reader *reader, uint8_t *bytes, size_t *length, unsigned timeout)
{
    birriasdr_dev_t *dev = reader->dev;
    struct model *model = dev->io.context;
    *length = 0;
    if (model->generated == model->limit || model->stopped) {
        bs_sleep_us(timeout * 1000);
        return BIRRIASDR_ERROR_TIMEOUT;
    }
    CHECK(model->started);
    memset(bytes, 0, BS_FRAME_BYTES);
    bs_put_le32(bytes, 0x38000000u | (BS_FRAME_BYTES - 4));
    bs_put_le32(bytes + 24, 0x38000014u + BS_PAYLOAD_BYTES);
    bytes[28] = 0xed;
    bytes[32] = 0xf5;
    bs_put_le32(bytes + 36, model->generated++);
    bs_put_le32(bytes + 40, dev->nonce);
    if (model->fault == BAD_FRAME) bytes[40] ^= 1;
    *length = BS_FRAME_BYTES;
    return 0;
}

/* Backend setup records that reset is needed even if preparation fails. */
static int prepare(birriasdr_dev_t *dev, uint32_t packets)
{
    struct model *model = dev->io.context;
    ++model->preparations;
    if (model->recovery_test) {
        if (!model->persistent && model->preparations > 1) model->fault = NONE;
        model->generated = 0;
        model->started = model->stopped = false;
    }
    dev->touched = true;
    dev->nonce = 0x12345678;
    dev->stats.stage = BIRRIASDR_STAGE_RF;
    model->limit = packets ? packets : model->recovery_test ? 2 : 100;
    model->terminal = packets ? 2 : 3;
    if (model->fault == CANCEL_PREPARE) CHECK(birriasdr_cancel_async(dev) == 0);
    if (model->fault == REMOVED) return BIRRIASDR_ERROR_NO_DEVICE;
    if (model->fault == RETAINED_PATCH) return BIRRIASDR_ERROR_COLD_BOOT;
    if (model->fault == RF_STATE) return BIRRIASDR_ERROR_STATE;
    return model->fault == PREPARE ? BIRRIASDR_ERROR_IO : 0;
}
static int start(birriasdr_dev_t *dev)
{
    struct model *model = dev->io.context;
    ++model->starts;
    model->started = true;
    return model->fault == START ? BIRRIASDR_ERROR_IO : 0;
}
static int stop(birriasdr_dev_t *dev)
{
    struct model *model = dev->io.context;
    ++model->stops;
    model->stopped = true;
    return model->fault == STOP || model->fault == TUNE_STOP_ERROR ? BIRRIASDR_ERROR_IO : 0;
}
static int status(birriasdr_dev_t *dev, uint32_t words[16])
{
    struct model *model = dev->io.context;
    memset(words, 0, 64);
    words[7] = model->stopped ? 3 : model->terminal;
    words[10] = words[11] = model->generated;
    if (model->fault == SOURCE_OVERRUN) {
        words[7] = 9;
        words[10] = words[11] = model->generated + 1;
    }
    if (model->fault == DMA_ERROR) {
        words[13] = 1;
        ++words[10];
    }
    return model->fault == STATUS ? BIRRIASDR_ERROR_IO : 0;
}
static int reset(birriasdr_dev_t *dev)
{
    struct model *model = dev->io.context;
    ++model->resets;
    CHECK(model->created == model->destroyed);
    return model->fault == RESET ? BIRRIASDR_ERROR_IO : 0;
}
static int validate(uint64_t frequency, uint32_t rate, const struct birriasdr_gain *gain)
{
    (void)gain;
    return frequency && rate ? 0 : BIRRIASDR_ERROR_INVALID;
}
static int gain(birriasdr_dev_t *dev, bool manual, const struct birriasdr_gain *codes)
{
    struct model *model = dev->io.context;
    (void)manual; (void)codes;
    return model->fault == GAIN_ERROR ? BIRRIASDR_ERROR_IO : 0;
}
static int tune(birriasdr_dev_t *dev, uint64_t frequency)
{
    struct model *model = dev->io.context;
    (void)frequency;
    model->tune_called = true;
    return model->fault == TUNE_STOP_ERROR ? BIRRIASDR_ERROR_STATE :
           model->fault == TUNE_ERROR ? BIRRIASDR_ERROR_IO : 0;
}

static const struct bs_backend backend = {
    .capabilities = BIRRIASDR_CAP_RAW_GAIN | BIRRIASDR_CAP_LIVE_GAIN | BIRRIASDR_CAP_LIVE_TUNING,
    .validate = validate, .prepare = prepare, .start = start, .stop = stop,
    .status = status, .reset = reset, .change_gain = gain, .change_frequency = tune
};

/* Public controls are legal inside callbacks; stop after a small validated
 * prefix. Closing a live handle must return BUSY without freeing anything.
 */
static void observe(const int16_t *iq, uint32_t count, uint64_t first, void *context)
{
    struct observation *observation = context;
    birriasdr_dev_t *dev = observation->dev;
    struct model *model = dev->io.context;
    CHECK(iq && count && first == observation->samples);
    observation->samples += count;
    if (observation->callbacks++ == 0) {
        CHECK(birriasdr_close(dev) == BIRRIASDR_ERROR_BUSY);
        CHECK(birriasdr_set_sample_rate(dev,80000000)==BIRRIASDR_ERROR_BUSY);
        if (model->fault == GAIN_ERROR)
            CHECK(birriasdr_set_tuner_gain_mode(dev, 1) == BIRRIASDR_ERROR_IO);
        else if (model->fault == TUNE_ERROR) {
            uint64_t old = birriasdr_get_center_freq(dev);
            CHECK(birriasdr_set_center_freq(dev, old + 29) == BIRRIASDR_ERROR_IO);
            CHECK(birriasdr_get_center_freq(dev) == old);
        }
    }
    if (observation->samples >= 6144) {
        CHECK(birriasdr_cancel_async(dev) == 0);
        CHECK(birriasdr_cancel_async(dev) == 0);
    }
}

/* Check every failure returns ownership to idle, destroys the reader, resets
 * touched hardware and permits a new finite read on that same device handle.
 */
static void test_case(enum fault fault, int expected)
{
    struct model model = {.fault = fault};
    birriasdr_dev_t dev = {.backend = &backend, .frequency = 2437000000, .rate = 40000000,
                          .control_mutex = PTHREAD_MUTEX_INITIALIZER};
    struct observation observation = {.dev = &dev};
    int16_t samples[4096];
    uint32_t received;
    int result;
    atomic_init(&dev.run_state, BS_IDLE);
    dev.io.context = &model;
    result = birriasdr_read_async(&dev, observe, &observation, 4, 2048);
    if (result != expected) fprintf(stderr, "fault=%d expected=%d actual=%d\n", fault, expected, result);
    CHECK(result == expected);
    CHECK(atomic_load(&dev.run_state) == BS_IDLE && !dev.control_ready);
    CHECK(model.created == model.destroyed);
    CHECK(model.resets == (fault == CREATE ? 0u : 1u));
    CHECK(dev.touched == (fault == RESET));
    if (fault == GAIN_ERROR) CHECK(dev.stats.stage == BIRRIASDR_STAGE_GAIN);
    if (fault == TUNE_ERROR) CHECK(dev.stats.stage == BIRRIASDR_STAGE_RF && model.tune_called);
    if (fault == SOURCE_OVERRUN) CHECK(dev.stats.firmware_state == 9);
    if (fault == DMA_ERROR) CHECK(dev.stats.dma_errors == 1);
    memset(&model, 0, sizeof(model));
    CHECK(birriasdr_read_sync(&dev, samples, 2048, &received) == 0);
    CHECK(received == 2048 && dev.stats.samples == 2048 && dev.stats.packets == 1);
    CHECK(!dev.control_error && !dev.touched && model.resets == 1);
    CHECK(pthread_mutex_destroy(&dev.control_mutex) == 0);
}

struct recovery_observation {
    birriasdr_dev_t *dev;
    uint64_t samples;
    unsigned gaps;
    bool cancel_in_gap, cancel_after_control;
};

/* A gap arrives after all reader storage has been destroyed and before any
 * samples of the next acquisition. Reentrant close/read/setters remain BUSY.
 */
static void gap(const struct birriasdr_recovery_event *event, void *context)
{
    struct recovery_observation *observation = context;
    birriasdr_dev_t *dev = observation->dev;
    struct model *model = dev->io.context;
    CHECK(event->attempt == ++observation->gaps && event->next_sample == observation->samples);
    CHECK(event->error < 0 && model->created == model->destroyed);
    CHECK(!dev->control_ready && atomic_load(&dev->run_state) == BS_RUNNING);
    CHECK(birriasdr_close(dev) == BIRRIASDR_ERROR_BUSY);
    CHECK(birriasdr_set_center_freq(dev, 2442000000) == BIRRIASDR_ERROR_BUSY);
    CHECK(birriasdr_set_recovery(dev, 0, NULL, NULL) == BIRRIASDR_ERROR_BUSY);
    if (model->fault == TUNE_STOP_ERROR) {
        CHECK(event->error == BIRRIASDR_ERROR_STATE);
        CHECK(event->previous.stage == BIRRIASDR_STAGE_RF);
    }
    if (observation->cancel_in_gap) CHECK(birriasdr_cancel_async(dev) == 0);
}

/* Offsets continue to count delivered samples across explicit discontinuities;
 * a failed control must retain the preceding committed center or gain mode.
 */
static void observe_recovery(const int16_t *iq, uint32_t count, uint64_t first, void *context)
{
    struct recovery_observation *observation = context;
    birriasdr_dev_t *dev = observation->dev;
    struct model *model = dev->io.context;
    CHECK(iq && count && first == observation->samples);
    observation->samples += count;
    if (model->fault == TUNE_ERROR || model->fault == TUNE_STOP_ERROR) {
        uint64_t frequency = birriasdr_get_center_freq(dev);
        CHECK(birriasdr_set_center_freq(dev, frequency + 29) ==
              (model->fault == TUNE_ERROR ? BIRRIASDR_ERROR_IO : BIRRIASDR_ERROR_STATE));
        CHECK(birriasdr_get_center_freq(dev) == frequency);
    } else if (model->fault == GAIN_ERROR) {
        CHECK(birriasdr_set_tuner_gain_mode(dev, 1) == BIRRIASDR_ERROR_IO);
        CHECK(!dev->manual);
    }
    if (model->fault == NONE || observation->cancel_after_control)
        CHECK(birriasdr_cancel_async(dev) == 0);
}

/* Recover one-shot failures, stop at the configured budget for persistent ones,
 * and never override explicit cancellation or retry a physical removal/patch.
 */
static void test_recovery(enum fault fault, bool persistent, bool cancel_gap,
                          bool cancel_control, bool finite, int expected, unsigned gaps)
{
    struct model model = {.fault = fault, .recovery_test = true, .persistent = persistent};
    birriasdr_dev_t dev = {.backend = &backend, .frequency = 2437000000, .rate = 40000000,
                          .control_mutex = PTHREAD_MUTEX_INITIALIZER};
    struct recovery_observation observation = {.dev = &dev, .cancel_in_gap = cancel_gap,
                                              .cancel_after_control = cancel_control};
    atomic_init(&dev.run_state, BS_IDLE);
    dev.io.context = &model;
    CHECK(birriasdr_set_recovery(&dev, 2, NULL, NULL) == BIRRIASDR_ERROR_INVALID);
    CHECK(birriasdr_set_recovery(&dev, 2, gap, &observation) == 0);
    int result;
    if (finite) {
        int16_t iq[4096];
        uint32_t received;
        result = birriasdr_read_sync(&dev, iq, 2048, &received);
    } else {
        result = birriasdr_read_async(&dev, observe_recovery, &observation, 4, 2048);
    }
    if (result != expected) fprintf(stderr, "recovery fault=%d result=%d expected=%d\n", fault, result, expected);
    CHECK(result == expected && observation.gaps == gaps);
    CHECK(model.preparations == 1 + gaps - (cancel_gap ? 1u : 0u));
    CHECK(model.created == model.destroyed);
    CHECK(model.resets == (fault == REMOVED ? 0u : model.preparations));
    CHECK(atomic_load(&dev.run_state) == BS_IDLE && !dev.control_ready);
    if (!finite) CHECK(dev.stats.samples == observation.samples);
    if (fault == REMOVED) {
        unsigned preparations = model.preparations, created = model.created;
        CHECK(dev.usb_lost && !dev.touched);
        CHECK(birriasdr_read_async(&dev, observe_recovery, &observation, 4, 2048) ==
              BIRRIASDR_ERROR_NO_DEVICE);
        CHECK(model.preparations == preparations && model.created == created && !model.resets);
    }
    CHECK(birriasdr_set_recovery(&dev, 0, NULL, NULL) == 0);
    CHECK(!dev.recovery_callback && !dev.recovery_limit);
    CHECK(pthread_mutex_destroy(&dev.control_mutex) == 0);
}

/* Run with no USB access. Each scenario drives the same implementation as users. */
int main(void)
{
    static const int expected[] = {0, -7, -5, -5, -5, -5, -5, -5, -10, -11, -10, -5, -5, 0};
    for (unsigned fault = 0; fault < sizeof(expected) / sizeof(expected[0]); ++fault)
        test_case((enum fault)fault, expected[fault]);
    const enum fault temporary[] = {PREPARE, SUBMIT, START, SOURCE_OVERRUN,
                                    GAIN_ERROR, TUNE_ERROR, TUNE_STOP_ERROR};
    for (size_t i = 0; i < sizeof(temporary) / sizeof(temporary[0]); ++i)
        test_recovery(temporary[i], false, false, false, false, 0, 1);
    test_recovery(PREPARE, true, false, false, false, BIRRIASDR_ERROR_IO, 2);
    test_recovery(SOURCE_OVERRUN, true, false, false, false, BIRRIASDR_ERROR_OVERRUN, 2);
    test_recovery(SOURCE_OVERRUN, false, true, false, false, BIRRIASDR_ERROR_OVERRUN, 1);
    test_recovery(TUNE_ERROR, false, false, true, false, BIRRIASDR_ERROR_IO, 0);
    test_recovery(PREPARE, false, false, false, true, BIRRIASDR_ERROR_IO, 0);
    test_recovery(REMOVED, false, false, false, false, BIRRIASDR_ERROR_NO_DEVICE, 0);
    test_recovery(RETAINED_PATCH, false, false, false, false, BIRRIASDR_ERROR_COLD_BOOT, 0);
    test_recovery(RF_STATE, false, false, false, false, BIRRIASDR_ERROR_STATE, 0);
    test_recovery(BAD_FRAME, false, false, false, false, BIRRIASDR_ERROR_PROTOCOL, 0);
    puts("stream lifecycle and recovery tests passed");
    return 0;
}
