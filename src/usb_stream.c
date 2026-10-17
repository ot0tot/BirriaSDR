/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */
#include "usb_stream.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct usb_buffer {
    struct usb_buffer *next;
    unsigned length;
    uint8_t *data;
};
struct transfer_slot {
    struct bs_reader *reader;
    struct libusb_transfer *transfer;
    bool pending;
    struct usb_buffer *buffer;
};

struct bs_reader {
    birriasdr_dev_t *dev;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t thread;
    bool started, stopping;
#ifdef _WIN32
    bool raw_io;
#endif
    int error;
    unsigned depth, inflight, capacity, head, tail, count, transfer_bytes;
    struct transfer_slot *slots;
    struct usb_buffer **queue, *buffers, *free_buffers;
    uint8_t *data_pool;
};

/* Cancel once with the queue lock held. Transfer memory remains alive until
 * cancellation callbacks have retired every submitted request.
 */
static void cancel_transfers(struct bs_reader *reader)
{
    if (reader->stopping)
        return;
    reader->stopping = true;
    for (unsigned i = 0; i < reader->depth; ++i)
        if (reader->slots[i].pending)
            libusb_cancel_transfer(reader->slots[i].transfer);
}

/* Completion callbacks hand buffers to a bounded queue and re-submit a spare.
 * RF control and application callbacks run outside libusb callbacks instead,
 * avoiding libusb event-lock recursion and preventing Python-style scheduling
 * gaps between individual USB reads. A full queue is an explicit error.
 */
static void LIBUSB_CALL receive_transfer(struct libusb_transfer *transfer)
{
    struct transfer_slot *slot = transfer->user_data;
    struct bs_reader *reader = slot->reader;
    pthread_mutex_lock(&reader->mutex);
    slot->pending = false;
    --reader->inflight;
    if (!reader->stopping && !reader->error) {
        if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
            reader->error = transfer->status == LIBUSB_TRANSFER_NO_DEVICE ?
                BIRRIASDR_ERROR_NO_DEVICE : BIRRIASDR_ERROR_IO;
        } else if (transfer->actual_length > 0) {
            uint64_t now = bs_now_ns();
            uint64_t gap = reader->dev->usb_diagnostics.last_ns ? now-reader->dev->usb_diagnostics.last_ns : 0;
            reader->dev->usb_diagnostics.last_ns = now;
            if (gap > reader->dev->usb_diagnostics.max_gap_ns) reader->dev->usb_diagnostics.max_gap_ns = gap;
            ++reader->dev->usb_diagnostics.completions;
            reader->dev->usb_diagnostics.bytes += (unsigned)transfer->actual_length;
            if (!reader->dev->usb_diagnostics.min_length || (unsigned)transfer->actual_length < reader->dev->usb_diagnostics.min_length)
                reader->dev->usb_diagnostics.min_length = (unsigned)transfer->actual_length;
            if ((unsigned)transfer->actual_length > reader->dev->usb_diagnostics.max_length)
                reader->dev->usb_diagnostics.max_length = (unsigned)transfer->actual_length;
            if (reader->count == reader->capacity) {
                reader->error = BIRRIASDR_ERROR_OVERRUN;
            } else {
                struct usb_buffer *completed = slot->buffer;
                completed->length = (unsigned)transfer->actual_length;
                reader->queue[reader->tail] = completed;
                slot->buffer = reader->free_buffers;
                reader->free_buffers = slot->buffer->next;
                transfer->buffer = slot->buffer->data;
                reader->tail = (reader->tail + 1) % reader->capacity;
                ++reader->count;
                if (reader->count > reader->dev->usb_diagnostics.queue_peak) reader->dev->usb_diagnostics.queue_peak = reader->count;
            }
        }
        if (!reader->error) {
            int result = libusb_submit_transfer(transfer);
            if (result < 0)
                reader->error = bs_usb_error(result);
            else {
                slot->pending = true;
                ++reader->inflight;
            }
        }
    }
    pthread_cond_broadcast(&reader->ready);
    pthread_mutex_unlock(&reader->mutex);
}

/* One event thread belongs to one handle/context. It keeps processing events
 * during shutdown until cancellation callbacks finish; freeing early is unsafe.
 */
static void *pump_events(void *context)
{
    struct bs_reader *reader = context;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    for (;;) {
        bool done;
        int result;
        struct timeval timeout = {0, 10000};
        pthread_mutex_lock(&reader->mutex);
        if (reader->error)
            cancel_transfers(reader);
        done = reader->stopping && reader->inflight == 0;
        pthread_mutex_unlock(&reader->mutex);
        if (done)
            return NULL;
        result = libusb_handle_events_timeout_completed(reader->dev->usb, &timeout, NULL);
        if (result < 0 && result != LIBUSB_ERROR_INTERRUPTED) {
            pthread_mutex_lock(&reader->mutex);
            if (!reader->error)
                reader->error = bs_usb_error(result);
            pthread_cond_broadcast(&reader->ready);
            pthread_mutex_unlock(&reader->mutex);
        }
    }
}

/* Allocate and touch the fixed pool before starting RF. The 80 MSps queue
 * provides extra consumer-pause headroom. Both rate profiles stay bounded.
 */
int bs_reader_create(birriasdr_dev_t *dev, unsigned depth, struct bs_reader **out)
{
    struct bs_reader *reader = calloc(1, sizeof(*reader));
    *out = NULL;
    if (!reader)
        return BIRRIASDR_ERROR_NO_MEMORY;
    reader->dev = dev;
    memset(&dev->usb_diagnostics, 0, sizeof(dev->usb_diagnostics));
    memset(dev->capture_diagnostics, 0, sizeof(dev->capture_diagnostics));
    reader->depth = depth;
    reader->capacity = depth < 64 ? depth * 8 : 512;
    reader->transfer_bytes = dev->rate == 80000000 ? BS_USB_BYTES : BS_USB_LOW_RATE_BYTES;
    if (pthread_mutex_init(&reader->mutex, NULL)) {
        free(reader);
        return BIRRIASDR_ERROR_IO;
    }
    if (pthread_cond_init(&reader->ready, NULL)) {
        pthread_mutex_destroy(&reader->mutex);
        free(reader);
        return BIRRIASDR_ERROR_IO;
    }
    reader->slots = calloc(depth, sizeof(*reader->slots));
    reader->queue = calloc(reader->capacity, sizeof(*reader->queue));
    /* One extra buffer lets the single consumer copy outside the queue lock
     * while every transfer and every queue slot still has its own storage. */
    reader->buffers = calloc((size_t)reader->capacity + depth + 1, sizeof(*reader->buffers));
    reader->data_pool = malloc(((size_t)reader->capacity + depth + 1) * reader->transfer_bytes);
    if (!reader->slots || !reader->queue || !reader->buffers || !reader->data_pool)
        goto allocation_failed;
    for (unsigned i=0;i<reader->capacity+depth+1;++i) {
        /* Touch data pages now, before GO makes page faults time-sensitive. */
        reader->buffers[i].data = reader->data_pool + (size_t)i * reader->transfer_bytes;
        memset(reader->buffers[i].data, 0, reader->transfer_bytes);
        reader->buffers[i].next = reader->free_buffers;
        reader->free_buffers = &reader->buffers[i];
    }
    for (unsigned i = 0; i < depth; ++i) {
        struct transfer_slot *slot = &reader->slots[i];
        slot->reader = reader;
        slot->transfer = libusb_alloc_transfer(0);
        if (!slot->transfer)
            goto allocation_failed;
        slot->buffer = reader->free_buffers;
        reader->free_buffers = slot->buffer->next;
        libusb_fill_bulk_transfer(slot->transfer, dev->handle, dev->ep_in, slot->buffer->data,
                                  (int)reader->transfer_bytes, receive_transfer, slot, 0);
    }
    *out = reader;
    return 0;
allocation_failed:
    bs_reader_destroy(reader);
    return BIRRIASDR_ERROR_NO_MEMORY;
}

/* Queue all transfers before the MCU GO write. On partial submission failure,
 * still run/drain the event pump so libusb retains no pointers into freed memory.
 */
int bs_reader_start(struct bs_reader *reader)
{
    int result = 0;
#ifdef _WIN32
    /* Bypass Windows bulk buffering so all reads can reach the USB controller.
     * Both read profiles are multiples of the SuperSpeed 1024-byte packet size.
     * Leave ROM traffic in normal mode, and restore it only after draining.
     */
    /* libusb 1.0.30's max-size query calls an unloaded GetPipePolicy pointer
     * with libusbK. Use bounded rate-specific transfers without that query.
     */
    if (libusb_endpoint_supports_raw_io(reader->dev->handle, reader->dev->ep_in) != 1)
        return BIRRIASDR_ERROR_UNSUPPORTED;
    result = libusb_endpoint_set_raw_io(reader->dev->handle, reader->dev->ep_in, 1);
    if (result < 0) return bs_usb_error(result);
    reader->raw_io = true;
#endif
    for (unsigned i = 0; i < reader->depth; ++i) {
        result = libusb_submit_transfer(reader->slots[i].transfer);
        if (result < 0)
            break;
        reader->slots[i].pending = true;
        ++reader->inflight;
    }
    if (result < 0)
        reader->error = bs_usb_error(result);
    if (pthread_create(&reader->thread, NULL, pump_events, reader)) {
        reader->error = BIRRIASDR_ERROR_IO;
        cancel_transfers(reader);
        pump_events(reader);
        return reader->error;
    }
    reader->started = true;
    return result < 0 ? bs_usb_error(result) : 0;
}

/* Detach a completed buffer under the lock, copy outside it, then return the
 * buffer to the pool. A callback cannot reuse it while the consumer copies.
 * CLOCK_REALTIME matches POSIX's default condition clock, including on macOS.
 */
int bs_reader_take(struct bs_reader *reader, uint8_t *data, size_t *length, unsigned timeout_ms)
{
    struct timespec deadline;
    struct usb_buffer *buffer = NULL;
    int result = 0;
    timespec_get(&deadline, TIME_UTC);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    deadline.tv_sec += deadline.tv_nsec / 1000000000;
    deadline.tv_nsec %= 1000000000;
    *length = 0;
    pthread_mutex_lock(&reader->mutex);
    while (!reader->count && !reader->error) {
        int wait = pthread_cond_timedwait(&reader->ready, &reader->mutex, &deadline);
        if (wait == ETIMEDOUT) {
            result = BIRRIASDR_ERROR_TIMEOUT;
            break;
        }
        if (wait) {
            result = BIRRIASDR_ERROR_IO;
            break;
        }
    }
    if (reader->error)
        result = reader->error;
    if (!result && reader->count) {
        buffer = reader->queue[reader->head];
        *length = buffer->length;
        reader->head = (reader->head + 1) % reader->capacity;
        --reader->count;
    }
    pthread_mutex_unlock(&reader->mutex);
    if (buffer) {
        memcpy(data, buffer->data, *length);
        pthread_mutex_lock(&reader->mutex);
        buffer->next = reader->free_buffers;
        reader->free_buffers = buffer;
        pthread_mutex_unlock(&reader->mutex);
    }
    return result;
}

/* Drain cancellations, join, then free. Also handles pre-start allocation errors. */
void bs_reader_destroy(struct bs_reader *reader)
{
    if (!reader)
        return;
    if (reader->started) {
        pthread_mutex_lock(&reader->mutex);
        cancel_transfers(reader);
        pthread_mutex_unlock(&reader->mutex);
        pthread_join(reader->thread, NULL);
    }
#ifdef _WIN32
    if (reader->raw_io)
        libusb_endpoint_set_raw_io(reader->dev->handle, reader->dev->ep_in, 0);
#endif
    if (reader->slots) {
        for (unsigned i = 0; i < reader->depth; ++i) {
            struct libusb_transfer *transfer = reader->slots[i].transfer;
            if (transfer) {
                libusb_free_transfer(transfer);
            }
        }
    }
    free(reader->slots);
    free(reader->queue);
    free(reader->buffers);
    free(reader->data_pool);
    pthread_cond_destroy(&reader->ready);
    pthread_mutex_destroy(&reader->mutex);
    free(reader);
}
