// SPDX-License-Identifier: GPL-2.0-only
// Adapted from px4-userland android_linkcheck.cpp at
// 1a1485d0c3e972e0a47be907edb67949564aa9a7; anchors ASICEN's USB adapter.
#include "asicen/libusb_transport.h"
#include <android/log.h>
#include <cstddef>
#include <libusb.h>

extern "C" void* memcpy(void*, const void*, std::size_t);

// Retained relocations force the adapter and official static libusb archive
// into the final ELF. They are never called: running this check is USB-inert.
[[gnu::used]] static volatile const auto kNativeInit = &asicen::initialize_libusb_context;
[[gnu::used]] static volatile const auto kLibusbInit = &libusb_init;
[[gnu::used]] static volatile const auto kLibusbWrap = &libusb_wrap_sys_device;
[[gnu::used]] static volatile const auto kLibusbClose = &libusb_close;
[[gnu::used]] static volatile const auto kAndroidLogPrint = &__android_log_print;
[[gnu::used]] static volatile const auto kLibcMemcpy = &memcpy;

int main()
{
    return (kNativeInit == nullptr || kLibusbInit == nullptr || kLibusbWrap == nullptr ||
            kLibusbClose == nullptr || kAndroidLogPrint == nullptr || kLibcMemcpy == nullptr)
               ? 1
               : 0;
}
