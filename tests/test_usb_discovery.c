/* SPDX-License-Identifier: MIT
 * Descriptor and ownership tests. No physical USB access.
 */
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static libusb_device *device_list[] = {(libusb_device *)(uintptr_t)1, NULL};
static struct libusb_endpoint_descriptor endpoints[8];
static struct libusb_interface_descriptor alternate[4];
static struct libusb_interface interfaces[4];
static struct libusb_config_descriptor config;
static unsigned opened, claimed, released, reads, closed;
static int speed, driver, claim_error;
static uint32_t chip;
static uint16_t vid, pid;

int LIBUSB_CALL libusb_init(libusb_context **context)
{ *context = (libusb_context *)(uintptr_t)1; return 0; }
void LIBUSB_CALL libusb_exit(libusb_context *context) { (void)context; }
ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *context, libusb_device ***list)
{ (void)context; *list = device_list; return 1; }
void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref)
{ (void)list; (void)unref; }
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *device, struct libusb_device_descriptor *desc)
{ (void)device; memset(desc, 0, sizeof(*desc)); desc->idVendor = vid; desc->idProduct = pid; return 0; }
int LIBUSB_CALL libusb_get_device_speed(libusb_device *device) { (void)device; return speed; }
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *device) { (void)device; return 250; }
uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *device) { (void)device; return 251; }
int LIBUSB_CALL libusb_open(libusb_device *device, libusb_device_handle **handle)
{ (void)device; ++opened; *handle = (libusb_device_handle *)(uintptr_t)1; return 0; }
void LIBUSB_CALL libusb_close(libusb_device_handle *handle) { (void)handle; ++closed; }
int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device *device, struct libusb_config_descriptor **out)
{ (void)device; *out = &config; return 0; }
void LIBUSB_CALL libusb_free_config_descriptor(struct libusb_config_descriptor *desc) { (void)desc; }
int LIBUSB_CALL libusb_kernel_driver_active(libusb_device_handle *handle, int interface_number)
{ (void)handle; CHECK(interface_number == 3); return driver; }
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *handle, int interface_number)
{ (void)handle; CHECK(interface_number == 3); ++claimed; return claim_error; }
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *handle, int interface_number)
{ (void)handle; CHECK(interface_number == 3); ++released; return 0; }
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *handle, uint8_t type,
    uint8_t request, uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout)
{
    (void)handle; (void)timeout;
    CHECK(type == 0xdf && request == 0x63 && value == 0x7001 && index == 0x0200 && length == 4);
    ++reads;
    bs_put_le32(data, chip);
    return 4;
}

static void setup(void)
{
    opened = claimed = released = reads = closed = 0;
    speed = LIBUSB_SPEED_SUPER; driver = 0; claim_error = 0;
    chip = 0x7961; vid = 0x0e8d; pid = 0x7961;
    memset(endpoints, 0, sizeof(endpoints));
    memset(alternate, 0, sizeof(alternate));
    for (unsigned i = 0; i < 4; ++i) {
        alternate[i].bInterfaceNumber = (uint8_t)i;
        alternate[i].bInterfaceClass = 0xe0; /* Bluetooth must never be claimed. */
        interfaces[i].num_altsetting = 1;
        interfaces[i].altsetting = &alternate[i];
    }
    alternate[3].bInterfaceClass = alternate[3].bInterfaceSubClass = alternate[3].bInterfaceProtocol = 0xff;
    alternate[3].bNumEndpoints = 8;
    alternate[3].endpoint = endpoints;
    for (unsigned i = 0; i < 8; ++i) {
        endpoints[i].bEndpointAddress = (uint8_t)(i < 2 ? 0x84 + i : i + 2);
        endpoints[i].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
    }
    config.bNumInterfaces = 4;
    config.interface = interfaces;
}

static int trial(uint32_t flags)
{
    birriasdr_dev_t dev = {0};
    dev.lock_fd = -1;
    dev.interface_number = -1;
    dev.open_flags = flags;
    int result = bs_usb_discover(0, NULL, &dev);
    if (!result) {
        CHECK(dev.interface_number == 3);
        CHECK(dev.ep_in == 0x84 && dev.ep_command == 4 && dev.ep_download == 5);
    }
    bs_usb_close(&dev);
    return result;
}

int main(void)
{
    const uint32_t flag = BIRRIASDR_OPEN_EXPERIMENTAL_MT7961;
    struct birriasdr_device_info info;
    setup();
    CHECK(bs_usb_discover(0, &info, NULL) == 0 && info.capabilities == 0);
    CHECK(opened == 0 && reads == 0);
    CHECK(trial(0) == BIRRIASDR_ERROR_UNSUPPORTED && opened == 0);
    CHECK(trial(flag) == 0 && claimed == 1 && released == 1 && reads == 1 && closed == 1);
    setup();
    CHECK(trial(flag|BIRRIASDR_OPEN_EXPERIMENTAL_TUNING)==0 && claimed==1 && released==1);
    setup(); speed = LIBUSB_SPEED_HIGH;
    CHECK(trial(flag) == BIRRIASDR_ERROR_UNSUPPORTED && opened == 0);
    setup(); driver = 1;
    CHECK(trial(flag) == BIRRIASDR_ERROR_BUSY && claimed == 0 && released == 0 && reads == 0);
    setup(); claim_error = LIBUSB_ERROR_BUSY;
    CHECK(trial(flag) == BIRRIASDR_ERROR_BUSY && released == 0 && reads == 0);
    setup(); driver = LIBUSB_ERROR_NOT_SUPPORTED; /* WinUSB has no detach API. */
    CHECK(trial(flag) == 0 && released == 1);
    setup(); chip = 0x7925;
    CHECK(trial(flag) == BIRRIASDR_ERROR_UNSUPPORTED && reads == 1 && released == 1);
    setup(); alternate[3].bNumEndpoints = 7;
    CHECK(trial(flag) == BIRRIASDR_ERROR_UNSUPPORTED && claimed == 0 && released == 0);
    setup(); alternate[2] = alternate[3];
    CHECK(trial(flag) == BIRRIASDR_ERROR_UNSUPPORTED && claimed == 0);
    setup(); pid = 0x7925;
    CHECK(trial(flag) == BIRRIASDR_ERROR_UNSUPPORTED && opened == 0);
    puts("USB discovery and composite interface ownership passed");
    return 0;
}
