/* SPDX-License-Identifier: GPL-2.0-only */
#include <libusb.h>

int main(void)
{
    const struct libusb_version *version = libusb_get_version();
    return version != NULL && version->nano == 12038 ? 0 : 1;
}
