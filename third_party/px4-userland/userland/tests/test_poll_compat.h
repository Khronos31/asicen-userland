// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_TEST_POLL_COMPAT_H
#define PX4_USERLAND_TEST_POLL_COMPAT_H

// Provides the POSIX poll shape used by the shared worker tests on Windows.
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

using pollfd = WSAPOLLFD;

#ifndef POLLIN
#define POLLIN POLLRDNORM
#endif

inline int test_poll(WSAPOLLFD* descriptors, unsigned long count, int timeout) noexcept
{
    return ::WSAPoll(descriptors, count, timeout);
}
#define poll test_poll

#else

#include <poll.h>

#endif

#endif  // PX4_USERLAND_TEST_POLL_COMPAT_H
