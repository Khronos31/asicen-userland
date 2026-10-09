// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/libusb_transport.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void kernel_driver_state_classification() {
    const auto active = asicen::classify_kernel_driver_state(1);
    check(active.known && active.active,
          "positive result is a known, bound kernel driver");

    const auto free_state = asicen::classify_kernel_driver_state(0);
    check(free_state.known && !free_state.active,
          "zero result is a known, driver-free interface");

    // Android libusb reports NOT_SUPPORTED; the interface is then known and
    // driver-free rather than unknown.
    const auto android =
        asicen::classify_kernel_driver_state(LIBUSB_ERROR_NOT_SUPPORTED);
    check(android.known && !android.active,
          "NOT_SUPPORTED is a known, driver-free state");

    const auto linux_error =
        asicen::classify_kernel_driver_state(LIBUSB_ERROR_NO_DEVICE);
    check(!linux_error.known && !linux_error.active,
          "other negative results keep the state unknown");
}

void duplicate_survives_closing_the_original() {
    int pipe_fds[2] = {-1, -1};
    check(::pipe(pipe_fds) == 0, "pipe creation failed");

    const char payload[] = "asicen";
    check(::write(pipe_fds[1], payload, sizeof(payload)) ==
              static_cast<ssize_t>(sizeof(payload)),
          "pipe write failed");

    const int duplicate = asicen::duplicate_fd_cloexec(pipe_fds[0]);
    check(duplicate >= 0, "a valid fd must duplicate");
    const int flags = ::fcntl(duplicate, F_GETFD);
    check(flags >= 0 && (flags & FD_CLOEXEC) != 0,
          "the duplicate must be close-on-exec");

    ::close(pipe_fds[0]);
    pipe_fds[0] = -1;

    char read_back[sizeof(payload)] = {};
    check(::read(duplicate, read_back, sizeof(read_back)) ==
              static_cast<ssize_t>(sizeof(read_back)),
          "the duplicate must stay readable after the original closes");
    check(std::memcmp(read_back, payload, sizeof(payload)) == 0,
          "the duplicate must return the original bytes");

    ::close(duplicate);
    ::close(pipe_fds[1]);
}

void invalid_fd_is_rejected() {
    check(asicen::duplicate_fd_cloexec(-1) < 0,
          "an invalid fd must not duplicate");
}

void failed_wrap_keeps_the_callers_fd_open() {
    libusb_context* context = nullptr;
    check(libusb_init(&context) == 0, "libusb_init failed");

    int pipe_fds[2] = {-1, -1};
    check(::pipe(pipe_fds) == 0, "pipe creation failed");

    asicen::LibusbDevice device;
    // A pipe is not a USB device, so the wrap fails after the internal
    // duplicate is made; only that duplicate may be cleaned up.
    check(device.open(context, pipe_fds[0]) != 0,
          "wrapping a non-USB fd must fail");
    check(::fcntl(pipe_fds[0], F_GETFD) >= 0,
          "the failure path must not close the caller's fd");
    check(!device.is_open(), "a failed open must not report an open device");

    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    libusb_exit(context);
}

void negative_descriptor_is_rejected() {
    libusb_context* context = nullptr;
    check(libusb_init(&context) == 0, "libusb_init failed");

    asicen::LibusbDevice device;
    check(device.open(context, -1) != 0, "a negative fd must be rejected");
    check(!device.is_open(), "a rejected fd must not open the device");

    libusb_exit(context);
}

}  // namespace

int main() {
    kernel_driver_state_classification();
    duplicate_survives_closing_the_original();
    invalid_fd_is_rejected();
    failed_wrap_keeps_the_callers_fd_open();
    negative_descriptor_is_rejected();
    std::cout << "libusb fd tests passed\n";
    return 0;
}
