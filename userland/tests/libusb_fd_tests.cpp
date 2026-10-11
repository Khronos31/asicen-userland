// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/libusb_transport.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace {

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,         \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

class ScopedFd final {
public:
    explicit ScopedFd(int fd) noexcept : fd_(fd) {}
    ~ScopedFd() noexcept { (void)close(); }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const noexcept { return fd_; }
    int close() noexcept
    {
        const int fd = fd_;
        fd_ = -1;
        return fd >= 0 ? ::close(fd) : 0;
    }

private:
    int fd_ = -1;
};

bool kernel_driver_state_classification()
{
    const auto active = asicen::classify_kernel_driver_state(1);
    CHECK(active.known && active.active);

    const auto free_state = asicen::classify_kernel_driver_state(0);
    CHECK(free_state.known && !free_state.active);

    // Android libusb reports NOT_SUPPORTED; the interface is then known and
    // driver-free rather than unknown.
    const auto android =
        asicen::classify_kernel_driver_state(LIBUSB_ERROR_NOT_SUPPORTED);
    CHECK(android.known && !android.active);

    const auto linux_error =
        asicen::classify_kernel_driver_state(LIBUSB_ERROR_NO_DEVICE);
    CHECK(!linux_error.known && !linux_error.active);
    return true;
}

bool duplicate_survives_closing_the_original()
{
    int pipe_fds[2] = {-1, -1};
    CHECK(::pipe(pipe_fds) == 0);
    ScopedFd original(pipe_fds[0]);
    ScopedFd writer(pipe_fds[1]);
    const int original_flags = ::fcntl(original.get(), F_GETFD);
    CHECK(original_flags >= 0);

    const char payload[] = "asicen";
    CHECK(::write(writer.get(), payload, sizeof(payload)) ==
          static_cast<ssize_t>(sizeof(payload)));

    ScopedFd duplicate(asicen::duplicate_fd_cloexec(original.get()));
    CHECK(duplicate.get() >= 0);
    const int flags = ::fcntl(duplicate.get(), F_GETFD);
    CHECK(flags >= 0 && (flags & FD_CLOEXEC) != 0);
    CHECK(::fcntl(original.get(), F_GETFD) == original_flags);

    CHECK(original.close() == 0);

    char read_back[sizeof(payload)] = {};
    CHECK(::read(duplicate.get(), read_back, sizeof(read_back)) ==
          static_cast<ssize_t>(sizeof(read_back)));
    CHECK(std::memcmp(read_back, payload, sizeof(payload)) == 0);

    CHECK(duplicate.close() == 0);
    CHECK(writer.close() == 0);
    return true;
}

bool invalid_fd_is_rejected()
{
    CHECK(asicen::duplicate_fd_cloexec(-1) < 0);
    return true;
}

bool failed_wrap_keeps_the_callers_fd_open()
{
    asicen::LibusbContext context;
    // Use the production FD-mode context: wrapping an already-granted fd does
    // not need device discovery or a host USB hotplug monitor in offline CI.
    CHECK(context.initialize(true) == 0);

    int pipe_fds[2] = {-1, -1};
    CHECK(::pipe(pipe_fds) == 0);
    ScopedFd original(pipe_fds[0]);
    ScopedFd writer(pipe_fds[1]);

    asicen::LibusbDevice device;
    // Linux/Android reject the pipe after duplicating it. Other POSIX hosts
    // reject USB-fd wrapping as unsupported. Neither path may close the caller.
    const int opened = device.open(context.get(), original.get());
    CHECK(opened != 0);
#if !defined(__linux__) && !defined(__ANDROID__)
    CHECK(opened == LIBUSB_ERROR_NOT_SUPPORTED);
#endif
    CHECK(::fcntl(original.get(), F_GETFD) >= 0);
    CHECK(!device.is_open());

    CHECK(original.close() == 0);
    CHECK(writer.close() == 0);
    return true;
}

bool negative_descriptor_is_rejected()
{
    asicen::LibusbContext context;
    CHECK(context.initialize(true) == 0);

    asicen::LibusbDevice device;
    const int opened = device.open(context.get(), -1);
    CHECK(opened != 0);
#if defined(__linux__) || defined(__ANDROID__)
    CHECK(opened == LIBUSB_ERROR_INVALID_PARAM);
#else
    CHECK(opened == LIBUSB_ERROR_NOT_SUPPORTED);
#endif
    CHECK(!device.is_open());

    return true;
}

}  // namespace

int main()
{
    if (!kernel_driver_state_classification() ||
        !duplicate_survives_closing_the_original() ||
        !invalid_fd_is_rejected() ||
        !failed_wrap_keeps_the_callers_fd_open() ||
        !negative_descriptor_is_rejected()) {
        return 1;
    }
    std::puts("libusb fd tests passed");
    return 0;
}
