/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 * A deliberately small recorder: no UI, DSP, device power cycling or hidden driver.
 */
#include "birriasdr.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t interrupted;
struct recorder { birriasdr_dev_t *dev; FILE *file; uint64_t limit, count; int error; };

/* Signal handlers only set a flag; library cancellation runs in normal context. */
static void on_signal(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

/* Write conventional little-endian signed I,Q, preserving all 14 source bits.
 * A requested sample limit is a file limit; cancellation still drains hardware.
 */
static void record(const int16_t *iq, uint32_t count, uint64_t first, void *context)
{
    struct recorder *recorder = context;
    uint8_t bytes[65536];
    (void)first;
    if (recorder->limit && count > recorder->limit - recorder->count)
        count = (uint32_t)(recorder->limit - recorder->count);
    for (uint32_t done = 0; done < count && !recorder->error;) {
        uint32_t n = count - done;
        if (n > sizeof(bytes) / 4)
            n = sizeof(bytes) / 4;
        for (uint32_t i = 0; i < n * 2; ++i) {
            uint16_t value = (uint16_t)iq[(size_t)done * 2 + i];
            bytes[2 * i] = (uint8_t)value;
            bytes[2 * i + 1] = (uint8_t)(value >> 8);
        }
        if (fwrite(bytes, 4, n, recorder->file) != n)
            recorder->error = 1;
        else
            recorder->count += n;
        done += n;
    }
    if (interrupted || recorder->error || (recorder->limit && recorder->count >= recorder->limit))
        birriasdr_cancel_async(recorder->dev);
}

/* List recognized hardware including unsupported boards, without opening them. */
static int list_devices(void)
{
    int count = birriasdr_get_device_count();
    if (count < 0) {
        fprintf(stderr, "%s\n", birriasdr_strerror(count));
        return 1;
    }
    for (int i = 0; i < count; ++i) {
        struct birriasdr_device_info info;
        if (birriasdr_get_device_info((uint32_t)i, &info))
            return 1;
        printf("%d: %04x:%04x %s [%s]\n", i, info.vid, info.pid, info.name,
               info.capabilities & BIRRIASDR_CAP_RX ? "RX implemented" : "unsupported");
    }
    return 0;
}

/* Parse decimal counts/Hz without accepting negative wrap or ignored suffixes. */
static int parse_number(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long number;
    if (*text < '0' || *text > '9')
        return -1;
    errno = 0;
    number = strtoull(text, &end, 10);
    if (errno || *end || !number || number > UINT64_MAX)
        return -1;
    *value = (uint64_t)number;
    return 0;
}

/* Record device 0, with optional center, finite count and raw manual gain codes.
 * Keep the example's flags small; applications can use the full public API.
 */
int main(int argc, char **argv)
{
    birriasdr_dev_t *dev = NULL;
    struct birriasdr_stream_stats stats;
    struct recorder recorder = {0};
    uint64_t frequency = 0;
    uint32_t device_index = 0, open_flags = 0;
    uint32_t rate=40000000, bandwidth=40000000;
    int result, close_result, argument = 1;
    while (argument < argc && argv[argument][0] == '-') {
        const char *option = argv[argument++];
        uint64_t index = 0;
        if (!strcmp(option, "--experimental-mt7961")) {
            open_flags |= BIRRIASDR_OPEN_EXPERIMENTAL_MT7961;
        } else if (!strcmp(option,"--experimental-tuning")) {
            open_flags |= BIRRIASDR_OPEN_EXPERIMENTAL_TUNING;
        } else if (!strcmp(option, "--list") && argument == argc) {
            return list_devices();
        } else if (!strcmp(option, "--frequency") && argument < argc) {
            if (parse_number(argv[argument++], &frequency)) return 1;
        } else if (!strcmp(option,"--rate") && argument<argc) {
            uint64_t value;
            if (parse_number(argv[argument++],&value) || (value!=40000000 && value!=80000000)) return 1;
            rate=(uint32_t)value;
            if (rate==80000000) open_flags|=BIRRIASDR_OPEN_EXPERIMENTAL_80MSPS;
        } else if (!strcmp(option,"--bandwidth") && argument<argc) {
            uint64_t value;
            if(parse_number(argv[argument++],&value) || (value!=40000000 && value!=80000000)) return 1;
            bandwidth=(uint32_t)value;
        } else if (!strcmp(option, "--device") && argument < argc) {
            if (strcmp(argv[argument], "0") && parse_number(argv[argument], &index)) return 1;
            if (index > UINT32_MAX) return 1;
            device_index = (uint32_t)index;
            ++argument;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", option);
            return 1;
        }
    }
    if (argc - argument < 1 || argc - argument > 3) {
        fprintf(stderr, "Usage: %s --list | [--experimental-mt7961] [--experimental-tuning] [--device index] [--frequency Hz] [--rate 40000000|80000000] [--bandwidth 40000000|80000000] output.cs16 [complex-samples [manual]]\n", argv[0]);
        return 1;
    }
    if (argc - argument >= 2 && parse_number(argv[argument + 1], &recorder.limit))
        return 1;
    if (argc - argument == 3 && strcmp(argv[argument + 2], "manual"))
        return 1;
    if (open_flags)
        fprintf(stderr, "Experimental 0e8d:7961 board profile enabled; RF behavior is unqualified.\n");
    result = birriasdr_open_ex(&dev, device_index, open_flags);
    if (result < 0) {
        fprintf(stderr, "Open: %s\n", birriasdr_strerror(result));
        return 1;
    }
    recorder.dev = dev;
    result=birriasdr_set_sample_rate(dev,rate);
    if (result<0) { fprintf(stderr,"Rate: %s\n",birriasdr_strerror(result));birriasdr_close(dev);return 1; }
    result=birriasdr_set_tuner_bandwidth(dev,bandwidth);
    if(result<0) { fprintf(stderr,"Bandwidth: %s\n",birriasdr_strerror(result));birriasdr_close(dev);return 1; }
    if (frequency) {
        result = birriasdr_set_center_freq(dev, frequency);
        if (result < 0) {
            fprintf(stderr, "Frequency: %s\n", birriasdr_strerror(result));
            birriasdr_close(dev);
            return 1;
        }
    }
    recorder.file = fopen(argv[argument], "wbx");
    if (!recorder.file) {
        perror("Output (must not already exist)");
        birriasdr_close(dev);
        return 1;
    }
    if (argc - argument == 3) {
        const struct birriasdr_gain gain = {2, 8, 0};
        result = birriasdr_set_tuner_gain_mode(dev, 1);
        if (!result)
            result = birriasdr_set_gain_codes(dev, &gain);
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (!result)
        result = birriasdr_read_async(dev, record, &recorder, 0, 0);
    birriasdr_get_stream_stats(dev, &stats);
    close_result = birriasdr_close(dev);
    if (fclose(recorder.file))
        recorder.error = 1;
    fprintf(stderr, "Saved %llu complex samples; stream=%s; firmware state=%u; stage=%d; reset=%s\n",
             (unsigned long long)recorder.count, birriasdr_strerror(result),
             stats.firmware_state, stats.stage, birriasdr_strerror(stats.reset_error));
    return result || close_result || recorder.error ? 1 : 0;
}
