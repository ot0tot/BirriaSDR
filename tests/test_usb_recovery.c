/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Luigi C.
 */

/* Model stale bulk input and USB reset outcomes after an interrupted capture.
 * Every transfer used here is mocked; no discovery or real libusb I/O occurs.
 * Oversized old events, timeout prefixes and persistent traffic must not be
 * mistaken for a clean ROM reply pipe. The drain has a finite work budget.
 */
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(t) do { if (!(t)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #t); exit(1); } } while (0)
struct response { int result, length; };
static struct {
    unsigned reads, resets;
    bool flood;
    int reset_error;
    struct response replies[8];
} model;
const struct bs_backend bs_mt7921_backend = {0}, bs_mt7925_backend = {0}, bs_mt7961_wide_backend = {0};

/* No actual port reset occurs; report each platform outcome explicitly. */
int LIBUSB_CALL libusb_reset_device(libusb_device_handle *handle)
{
    (void)handle;
    ++model.resets;
    return model.reset_error;
}

/* Proactive halt clearing broke the physical device; never reintroduce it. */
int LIBUSB_CALL libusb_clear_halt(libusb_device_handle *handle, unsigned char endpoint)
{
    (void)handle; (void)endpoint;
    abort();
}

/* A 16 KiB stale event would overflow the original 4 KiB ROM reply buffer.
 * Timeout prefixes count as data; only a timeout with zero bytes proves quiet.
 */
int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
                                    unsigned char *data, int size, int *actual, unsigned timeout)
{
    (void)handle;
    CHECK(endpoint == 0x81 && size == 65536 && timeout == 10);
    unsigned index = model.reads++;
    CHECK(model.flood || index < 8);
    struct response reply = model.flood ? (struct response){0, size} : model.replies[index];
    CHECK(reply.length >= 0 && reply.length <= size);
    *actual = reply.length;
    memset(data, 0xa5, (size_t)*actual);
    return reply.result;
}

int main(void)
{
    birriasdr_dev_t dev = {.ep_in = 0x81, .ep_command = 2, .ep_download = 3};
    model.replies[0] = (struct response){0, 16384};
    model.replies[1] = (struct response){LIBUSB_ERROR_TIMEOUT, 64};
    model.replies[2] = (struct response){LIBUSB_ERROR_OVERFLOW, 0};
    model.replies[3] = (struct response){LIBUSB_ERROR_TIMEOUT, 0};
    CHECK(bs_usb_prepare_endpoints(&dev) == 0);
    CHECK(model.reads == 4);
    const int errors[] = {0, LIBUSB_ERROR_NOT_FOUND, LIBUSB_ERROR_NO_DEVICE,
                          LIBUSB_ERROR_ACCESS, LIBUSB_ERROR_IO};
    const int expected[] = {0, BIRRIASDR_ERROR_NO_DEVICE, BIRRIASDR_ERROR_NO_DEVICE,
                            BIRRIASDR_ERROR_ACCESS, BIRRIASDR_ERROR_IO};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        memset(&model, 0, sizeof(model));
        dev.usb_lost = false;
        dev.touched = true;
        model.reset_error = errors[i];
        CHECK(bs_usb_recover(&dev) == expected[i]);
        CHECK(model.resets == 1 && model.reads == 0);
        if (expected[i] == BIRRIASDR_ERROR_NO_DEVICE) {
            CHECK(dev.usb_lost && !dev.touched);
            CHECK(bs_usb_recover(&dev) == BIRRIASDR_ERROR_NO_DEVICE);
            CHECK(model.resets == 1);
        }
    }
    memset(&model, 0, sizeof(model));
    model.replies[0] = (struct response){LIBUSB_ERROR_PIPE, 0};
    CHECK(bs_usb_prepare_endpoints(&dev) == BIRRIASDR_ERROR_IO);
    CHECK(model.reads == 1);
    memset(&model, 0, sizeof(model));
    model.flood = true;
    CHECK(bs_usb_prepare_endpoints(&dev) == BIRRIASDR_ERROR_TIMEOUT);
    CHECK(model.reads > 0 && model.reads <= 64);
    puts("USB pipe cleanup and stale-event tests passed");
    return 0;
}
