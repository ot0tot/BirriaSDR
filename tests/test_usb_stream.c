/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 * Fake libusb completions test ownership; this executable never opens USB.
 */
#include "usb_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); abort(); } } while (0)
struct request { struct libusb_transfer *transfer; bool cancelled; };
static struct {
    pthread_mutex_t mutex;
    struct request pending[256];
    unsigned count, allocated, freed, submitted, fail_submit, completions, limit, length;
    unsigned allocations, fail_allocation;
    enum libusb_transfer_status status;
} mock = {.mutex = PTHREAD_MUTEX_INITIALIZER};

#ifdef _WIN32
static bool raw_io;
static int raw_support = 1, raw_error;
int LIBUSB_CALL libusb_endpoint_supports_raw_io(libusb_device_handle *dev, uint8_t ep)
{ (void)dev; (void)ep; return raw_support; }
int LIBUSB_CALL libusb_endpoint_set_raw_io(libusb_device_handle *dev, uint8_t ep, int enable)
{
    (void)dev; (void)ep;
    CHECK(mock.count == 0);
    if (enable && raw_error) return raw_error;
    CHECK(raw_io != (enable != 0));
    raw_io = enable != 0;
    return 0;
}
#endif

#ifdef TEST_WRAP_ALLOCATIONS
/* GNU ld wrappers inject each reader allocation failure without adding hooks
 * to production code. Only arm them during single-threaded reader creation.
 */
static unsigned allocation_call, allocation_failure;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__wrap_malloc(size_t size)
{
    if (allocation_failure && ++allocation_call == allocation_failure) return NULL;
    return __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size)
{
    if (allocation_failure && ++allocation_call == allocation_failure) return NULL;
    return __real_calloc(count, size);
}
#endif

/* Allocate the same public transfer layout used by libusb_fill_bulk_transfer. */
struct libusb_transfer * LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    struct libusb_transfer *transfer;
    CHECK(iso_packets == 0);
    if (++mock.allocations == mock.fail_allocation)
        return NULL;
    transfer = calloc(1, sizeof(*transfer));
    if (transfer) ++mock.allocated;
    return transfer;
}

/* Freeing a transfer still owned by the event loop is a test failure. */
void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&mock.mutex);
    for (unsigned i = 0; i < mock.count; ++i)
        CHECK(mock.pending[i].transfer != transfer);
    ++mock.freed;
    pthread_mutex_unlock(&mock.mutex);
    free(transfer);
}

/* Inject either a partial-start failure or a failure during callback resubmission. */
int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
#ifdef _WIN32
    CHECK(raw_io);
#endif
    pthread_mutex_lock(&mock.mutex);
    if (++mock.submitted == mock.fail_submit) {
        pthread_mutex_unlock(&mock.mutex);
        return LIBUSB_ERROR_IO;
    }
    CHECK(mock.count < 256);
    mock.pending[mock.count++] = (struct request){transfer, false};
    pthread_mutex_unlock(&mock.mutex);
    return 0;
}

/* Cancellation marks a request; it deliberately does not complete it yet. */
int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    pthread_mutex_lock(&mock.mutex);
    for (unsigned i = 0; i < mock.count; ++i) {
        if (mock.pending[i].transfer == transfer) {
            mock.pending[i].cancelled = true;
            pthread_mutex_unlock(&mock.mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&mock.mutex);
    return LIBUSB_ERROR_NOT_FOUND;
}

/* Deliver data or deferred cancellation on the actual reader thread, exercising
 * callback/main-thread locking. No completion may outlive reader destruction.
 */
int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *context,
                                                       struct timeval *timeout, int *completed)
{
    struct libusb_transfer *transfer = NULL;
    (void)context;
    (void)timeout;
    (void)completed;
    pthread_mutex_lock(&mock.mutex);
    for (unsigned i = 0; i < mock.count; ++i) {
        struct request request = mock.pending[i];
        if (!request.cancelled && mock.completions >= mock.limit)
            continue;
        transfer = request.transfer;
        memmove(&mock.pending[i], &mock.pending[i + 1], (mock.count - i - 1) * sizeof(request));
        --mock.count;
        transfer->status = request.cancelled ? LIBUSB_TRANSFER_CANCELLED : mock.status;
        transfer->actual_length = request.cancelled ? 0 : (int)mock.length;
        if (!request.cancelled)
            memset(transfer->buffer, (int)++mock.completions, mock.length);
        break;
    }
    pthread_mutex_unlock(&mock.mutex);
    if (transfer)
        transfer->callback(transfer);
    else
        bs_sleep_us(1000);
    return 0;
}

/* Reset only after all previous transfers have completed and been freed. */
static void reset_mock(unsigned limit, unsigned fail_submit)
{
#ifdef _WIN32
    CHECK(!raw_io);
    raw_support = 1;
    raw_error = 0;
#endif
    CHECK(mock.count == 0 && mock.allocated == mock.freed);
    mock.allocated = mock.freed = mock.submitted = mock.completions = 0;
    mock.allocations = mock.fail_allocation = 0;
    mock.status = LIBUSB_TRANSFER_COMPLETED;
    mock.limit = limit;
    mock.length = 16;
    mock.fail_submit = fail_submit;
}

/* Clean shutdown, consumer timeout, partial startup and resubmit failure must
 * all leave zero in-flight transfers and exactly one free per allocation.
 */
int main(void)
{
    birriasdr_dev_t dev = {0};
    struct bs_reader *reader;
    uint8_t bytes[BS_USB_BYTES];
    size_t length;
#ifdef TEST_WRAP_ALLOCATIONS
    for (unsigned i = 1; i <= 9; ++i) {
        reset_mock(0, 0);
        allocation_call = 0;
        allocation_failure = i;
        CHECK(bs_reader_create(&dev, 4, &reader) == BIRRIASDR_ERROR_NO_MEMORY);
        allocation_failure = 0;
        CHECK(reader == NULL && mock.allocated == mock.freed);
    }
#endif
    for (unsigned i = 1; i <= 4; ++i) {
        reset_mock(0, 0);
        mock.fail_allocation = i;
        CHECK(bs_reader_create(&dev, 4, &reader) == BIRRIASDR_ERROR_NO_MEMORY);
        CHECK(reader == NULL && mock.allocated == mock.freed);
    }
#ifdef _WIN32
    reset_mock(0, 0);
    raw_support = 0;
    CHECK(bs_reader_create(&dev, 4, &reader) == 0);
    CHECK(bs_reader_start(reader) == BIRRIASDR_ERROR_UNSUPPORTED);
    bs_reader_destroy(reader);
    reset_mock(0, 0);
    raw_error = LIBUSB_ERROR_IO;
    CHECK(bs_reader_create(&dev, 4, &reader) == 0);
    CHECK(bs_reader_start(reader) == BIRRIASDR_ERROR_IO);
    bs_reader_destroy(reader);
#endif
    reset_mock(8, 0);
    CHECK(bs_reader_create(&dev, 4, &reader) == 0);
    CHECK(bs_reader_start(reader) == 0);
    for (unsigned i = 1; i <= 8; ++i) {
        CHECK(bs_reader_take(reader, bytes, &length, 1000) == 0);
        CHECK(length == 16 && bytes[0] == i && bytes[15] == i);
    }
    CHECK(bs_reader_take(reader, bytes, &length, 5) == BIRRIASDR_ERROR_TIMEOUT);
    bs_reader_destroy(reader);
    /* Recycle whole buffers repeatedly. A queued completion may not alias a
     * resubmitted transfer, even while the consumer copies outside the lock. */
    reset_mock(0,0);
    dev.rate=80000000;
    mock.length=BS_USB_BYTES;
    CHECK(bs_reader_create(&dev,4,&reader)==0);
    CHECK(bs_reader_start(reader)==0);
    for (unsigned i=1;i<=300;++i) {
        pthread_mutex_lock(&mock.mutex);mock.limit=i;pthread_mutex_unlock(&mock.mutex);
        CHECK(bs_reader_take(reader,bytes,&length,1000)==0 && length==BS_USB_BYTES);
        for (unsigned j=0;j<length;++j) CHECK(bytes[j]==(uint8_t)i);
    }
    bs_reader_destroy(reader);
    for (unsigned i = 1; i <= 4; ++i) {
        reset_mock(0, i);
        CHECK(bs_reader_create(&dev, 4, &reader) == 0);
        CHECK(bs_reader_start(reader) == BIRRIASDR_ERROR_IO);
        bs_reader_destroy(reader);
    }
    reset_mock(8, 5);
    CHECK(bs_reader_create(&dev, 4, &reader) == 0);
    CHECK(bs_reader_start(reader) == 0);
    CHECK(bs_reader_take(reader, bytes, &length, 1000) == BIRRIASDR_ERROR_IO);
    bs_reader_destroy(reader);
    for (unsigned i = 0; i < 2; ++i) {
        reset_mock(1, 0);
        mock.status = i ? LIBUSB_TRANSFER_ERROR : LIBUSB_TRANSFER_NO_DEVICE;
        CHECK(bs_reader_create(&dev, 4, &reader) == 0);
        CHECK(bs_reader_start(reader) == 0);
        CHECK(bs_reader_take(reader, bytes, &length, 1000) ==
              (i ? BIRRIASDR_ERROR_IO : BIRRIASDR_ERROR_NO_DEVICE));
        bs_reader_destroy(reader);
    }
    reset_mock(1000, 0);
    CHECK(bs_reader_create(&dev, 4, &reader) == 0);
    CHECK(bs_reader_start(reader) == 0);
    uint64_t deadline = bs_now_ns() + UINT64_C(1000000000);
    for (;;) {
        unsigned completed;
        pthread_mutex_lock(&mock.mutex);
        completed = mock.completions;
        pthread_mutex_unlock(&mock.mutex);
        if (completed > 32) break;
        CHECK(bs_now_ns() < deadline);
        bs_sleep_us(1000);
    }
    CHECK(bs_reader_take(reader, bytes, &length, 1000) == BIRRIASDR_ERROR_OVERRUN);
    bs_reader_destroy(reader);
    reset_mock(0, 0);
    puts("USB transfer ownership and failure tests passed");
    return 0;
}
