/* SPDX-License-Identifier: MIT
 * Latest-window display handoff. The USB consumer never calls Python and
 * never waits for a display lock. Skipped windows are display loss only.
 */
#include "usb_stream.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define DISPLAY_API __declspec(dllexport)
#else
#define DISPLAY_API __attribute__((visibility("default")))
#endif
#define DISPLAY_CAPACITY 262144u

struct display_bridge {
    pthread_mutex_t mutex;
    int16_t *iq;
    uint32_t size;
    uint64_t first, generation;
    bool ready;
    atomic_uint_fast64_t samples, blocks, published, busy, overwritten, next_ns;
};

static uint64_t display_now_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency;
    QueryPerformanceCounter(&ticks); QueryPerformanceFrequency(&frequency);
    return (uint64_t)(ticks.QuadPart / frequency.QuadPart) * UINT64_C(1000000000)
        + (uint64_t)(ticks.QuadPart % frequency.QuadPart) * UINT64_C(1000000000)
          / (uint64_t)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
#endif
}

DISPLAY_API void *bs_display_create(void)
{
    struct display_bridge *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->iq = calloc(DISPLAY_CAPACITY * 2, sizeof(*b->iq));
    if (!b->iq || pthread_mutex_init(&b->mutex, NULL)) {
        free(b->iq); free(b); return NULL;
    }
    /* Touch all pages before RF starts. */
    memset(b->iq, 0, DISPLAY_CAPACITY * 2 * sizeof(*b->iq));
    atomic_init(&b->samples, 0); atomic_init(&b->blocks, 0);
    atomic_init(&b->published, 0); atomic_init(&b->busy, 0);
    atomic_init(&b->overwritten, 0); atomic_init(&b->next_ns, 0);
    return b;
}

DISPLAY_API void bs_display_destroy(void *context)
{
    struct display_bridge *b = context;
    if (!b) return;
    /* Owner joins BOTH the native reader and display worker first. */
    pthread_mutex_destroy(&b->mutex);
    free(b->iq); free(b);
}

DISPLAY_API int bs_display_configure(void *context, uint32_t size,
                                     uint64_t generation, int reset_counters)
{
    struct display_bridge *b = context;
    if (!b || !size || size > DISPLAY_CAPACITY) return BIRRIASDR_ERROR_INVALID;
    pthread_mutex_lock(&b->mutex);
    b->size = size; b->generation = generation; b->ready = false;
    atomic_store(&b->next_ns, 0);
    if (reset_counters) {
        atomic_store(&b->samples, 0); atomic_store(&b->blocks, 0);
        atomic_store(&b->published, 0); atomic_store(&b->busy, 0);
        atomic_store(&b->overwritten, 0);
    }
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

DISPLAY_API void bs_display_callback(const int16_t *iq, uint32_t count,
                                     uint64_t first, void *context)
{
    struct display_bridge *b = context;
    uint64_t now;
    if (!b) return;
    atomic_fetch_add(&b->samples, count);
    atomic_fetch_add(&b->blocks, 1);
    now = display_now_ns();
    if (now < atomic_load(&b->next_ns)) return;
    /* A slow reader, FFT or HTTP request must never delay native draining. */
    if (pthread_mutex_trylock(&b->mutex)) {
        atomic_fetch_add(&b->busy, 1); return;
    }
    if (b->size && count >= b->size) {
        if (b->ready) atomic_fetch_add(&b->overwritten, 1);
        memcpy(b->iq, iq + 2u * (count - b->size), b->size * 2u * sizeof(*iq));
        b->first = first + count - b->size;
        b->ready = true;
        atomic_store(&b->next_ns, now + (b->size == DISPLAY_CAPACITY ?
                     UINT64_C(100000000) : UINT64_C(50000000)));
        atomic_fetch_add(&b->published, 1);
    }
    pthread_mutex_unlock(&b->mutex);
}

DISPLAY_API int bs_display_take(void *context, int16_t *iq, uint32_t capacity,
                                uint32_t *count, uint64_t *first, uint64_t *generation)
{
    struct display_bridge *b = context;
    int result = 0;
    if (!b || !iq || !count || !first || !generation) return BIRRIASDR_ERROR_INVALID;
    pthread_mutex_lock(&b->mutex);
    if (b->ready) {
        if (capacity < b->size) result = BIRRIASDR_ERROR_INVALID;
        else {
            memcpy(iq, b->iq, b->size * 2u * sizeof(*iq));
            *count = b->size; *first = b->first; *generation = b->generation;
            b->ready = false; result = 1;
        }
    }
    pthread_mutex_unlock(&b->mutex);
    return result;
}

DISPLAY_API void bs_display_counters(void *context, uint64_t out[5])
{
    struct display_bridge *b = context;
    out[0] = atomic_load(&b->samples); out[1] = atomic_load(&b->blocks);
    out[2] = atomic_load(&b->published); out[3] = atomic_load(&b->busy);
    out[4] = atomic_load(&b->overwritten);
}

DISPLAY_API int bs_display_read(birriasdr_dev_t *dev, void *context)
{
#ifdef _WIN32
    HANDLE thread = GetCurrentThread();
    int priority = GetThreadPriority(thread);
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    int result = birriasdr_read_async(dev, bs_display_callback, context, 0, DISPLAY_CAPACITY);
#ifdef _WIN32
    if (priority != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(thread, priority);
#endif
    return result;
}

/* Private diagnostics: this companion is built with the SAME native sources.
 * Read only after read_async returns. No USB register reads during capture. */
DISPLAY_API int bs_display_diagnostics_v2(birriasdr_dev_t *dev, uint64_t out[10])
{
    if (!dev || !out) return BIRRIASDR_ERROR_INVALID;
    if (atomic_load(&dev->run_state) != BS_IDLE) return BIRRIASDR_ERROR_BUSY;
    out[0] = dev->usb_diagnostics.queue_peak;
    out[1] = dev->usb_diagnostics.max_gap_ns;
    out[2] = dev->usb_diagnostics.completions;
    out[3] = dev->capture_diagnostics[4];
    out[4] = dev->capture_diagnostics[5];
    out[5] = 512;
    out[6] = dev->usb_diagnostics.min_length;
    out[7] = dev->usb_diagnostics.max_length;
    out[8] = atomic_load(&dev->rate) == 80000000 ? BS_USB_BYTES : BS_USB_LOW_RATE_BYTES;
    out[9] = atomic_load(&dev->rate) == 80000000 ? 232 : 32;
    return 0;
}
