/* SPDX-License-Identifier: MIT */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../integrations/standalone/stream_bridge.c"
#include <assert.h>
#include <stdio.h>

static void *contended_producer(void *context)
{
    int16_t iq[32] = {0};
    bs_display_callback(iq, 16, 100, context);
    return NULL;
}

int main(void)
{
    struct display_bridge *b = bs_display_create();
    uint64_t counters[5], first = 0, generation = 0;
    uint32_t count = 0;
    int16_t source[32], result[32] = {0};
    assert(b);
    for (unsigned i = 0; i < 32; ++i) source[i] = (int16_t)i;
    assert(bs_display_configure(b, 8, 42, 1) == 0);
    bs_display_callback(source, 16, 100, b);
    memset(source, 0, sizeof(source));
    assert(bs_display_take(b, result, 4, &count, &first, &generation) == BIRRIASDR_ERROR_INVALID);
    assert(bs_display_take(b, result, 16, &count, &first, &generation) == 1);
    assert(count == 8 && first == 108 && generation == 42);
    for (unsigned i = 0; i < 16; ++i) assert(result[i] == (int16_t)(i + 16));
    assert(bs_display_take(b, result, 16, &count, &first, &generation) == 0);

    /* A consumer can hold the mailbox while the USB consumer finishes its
     * callback. This must skip a DISPLAY window, not wait or drop samples. */
    atomic_store(&b->next_ns, 0);
    pthread_mutex_lock(&b->mutex);
    pthread_t producer;
    assert(pthread_create(&producer, NULL, contended_producer, b) == 0);
    assert(pthread_join(producer, NULL) == 0);
    pthread_mutex_unlock(&b->mutex);
    bs_display_counters(b, counters);
    assert(counters[0] == 32 && counters[1] == 2 && counters[2] == 1 && counters[3] == 1);
    assert(bs_display_configure(b, 0, 43, 0) == BIRRIASDR_ERROR_INVALID);
    assert(bs_display_configure(b, 8, 43, 0) == 0);
    bs_display_callback(source, 16, 200, b);
    atomic_store(&b->next_ns, 0);
    bs_display_callback(source, 16, 300, b);
    bs_display_counters(b, counters);
    assert(counters[4] == 1);
    assert(bs_display_take(b, result, 16, &count, &first, &generation) == 1);
    assert(first == 308 && generation == 43);
    bs_display_destroy(b);
    puts("Native display ownership and nonblocking producer checks passed.");
    return 0;
}
