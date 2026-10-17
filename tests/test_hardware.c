/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 * Explicit hardware smoke test, never run automatically by CTest.
 */
#include "birriasdr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct observation { birriasdr_dev_t *dev; uint64_t samples; int error; };

/* Exercise callback ordering, safe cancellation and a live gain-mode change.
 * Do no file I/O during streaming, so this tests the C library delivery path.
 */
static void observe(const int16_t *iq, uint32_t count, uint64_t first, void *context)
{
    struct observation *observation = context;
    if (first != observation->samples || iq[0] < -8192 || iq[0] > 8191)
        observation->error = 1;
    if (!first && birriasdr_set_tuner_gain_mode(observation->dev, 0) != 0)
        observation->error = 1;
    observation->samples += count;
    if (observation->samples >= 40000000 || observation->error)
        birriasdr_cancel_async(observation->dev);
}

/* Require explicit --run. Test repeated captures, both gain modes and clean
 * cancellation on one handle; prints enough terminal evidence for a test log.
 */
int main(int argc, char **argv)
{
    birriasdr_dev_t *dev = NULL;
    const uint32_t samples = 3906u * 2048;
    const struct birriasdr_gain gain = {2, 8, 0};
    struct birriasdr_stream_stats stats;
    struct observation observation = {0};
    uint32_t received;
    uint32_t open_flags = 0;
    int result = 0, close_result;
    int16_t *iq;
    if (argc == 3 && !strcmp(argv[2], "--experimental-mt7961"))
        open_flags = BIRRIASDR_OPEN_EXPERIMENTAL_MT7961;
    if ((argc != 2 && !(argc == 3 && open_flags)) || strcmp(argv[1], "--run")) {
        puts("Pass --run [--experimental-mt7961] to claim/reset device 0 and capture with original firmware.");
        return 1;
    }
    iq = malloc((size_t)samples * 4);
    if (!iq)
        return 1;
    result = birriasdr_open_ex(&dev, 0, open_flags);
    if (result < 0)
        goto cleanup;
    for (int manual = 1; manual >= 0; --manual) {
        result = birriasdr_set_tuner_gain_mode(dev, manual);
        if (!result && manual)
            result = birriasdr_set_gain_codes(dev, &gain);
        if (result < 0)
            goto cleanup;
        result = birriasdr_read_sync(dev, iq, samples, &received);
        birriasdr_get_stream_stats(dev, &stats);
        printf("sync manual=%d result=%d samples=%u state=%u dma=%u reset=%d stage=%d\n",
                manual, result, received, stats.firmware_state, stats.dma_errors, stats.reset_error, stats.stage);
        if (result < 0 || received != samples || stats.firmware_state != 2 || stats.reset_error) {
            result = result ? result : BIRRIASDR_ERROR_STATE;
            goto cleanup;
        }
    }
    observation.dev = dev;
    result = birriasdr_read_async(dev, observe, &observation, 0, 0);
    birriasdr_get_stream_stats(dev, &stats);
    printf("async result=%d samples=%llu state=%u dma=%u reset=%d callback_error=%d\n",
            result, (unsigned long long)observation.samples, stats.firmware_state,
            stats.dma_errors, stats.reset_error, observation.error);
    if (!result && (stats.firmware_state != 3 || observation.error || stats.reset_error))
        result = BIRRIASDR_ERROR_STATE;
cleanup:
    close_result = dev ? birriasdr_close(dev) : 0;
    free(iq);
    if (result || close_result)
        fprintf(stderr, "capture=%s close=%s\n", birriasdr_strerror(result), birriasdr_strerror(close_result));
    return result || close_result ? 1 : 0;
}
