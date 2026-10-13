// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <chrono>

namespace asicen::cli::loader_acquisition {

enum class Attempt { Acquired, Retry, Failed };
enum class Result { Acquired, Failed, Cancelled, TimedOut };

// Stable libusb error values, checked against its declarations by the caller.
// Keeping the classifier independent of libusb makes the exact policy testable.
inline constexpr int kOpenIo = -1;
inline constexpr int kOpenNoDevice = -4;
inline constexpr int kOpenNotFound = -5;
inline constexpr bool retryable_open_error(int result) noexcept
{
    return result == kOpenIo || result == kOpenNoDevice || result == kOpenNotFound;
}

template<class Error>
constexpr bool retryable_resolve_error(Error result) noexcept
{
    return result == Error::NOT_FOUND || result == Error::NOT_READY;
}

// Only resolution and opening belong in acquire. Identity checks, claims and
// firmware transfers run once after acquisition, never inside this retry loop.
// Injected monotonic time and waiting keep the deadline testable without USB.
template<class Clock, class Cancelled, class Acquire, class Wait>
Result run(bool retry, Clock now, Cancelled cancelled, Acquire acquire, Wait wait)
{
    using SteadyClock = std::chrono::steady_clock;
    const auto deadline = now() + std::chrono::seconds(10);
    const SteadyClock::duration interval = std::chrono::milliseconds(250);
    for (;;) {
        if (cancelled()) return Result::Cancelled;
        if (retry && now() >= deadline) return Result::TimedOut;
        const Attempt attempt = acquire();
        if (attempt == Attempt::Acquired) return Result::Acquired;
        if (attempt == Attempt::Failed || !retry) return Result::Failed;
        if (cancelled()) return Result::Cancelled;
        const auto current = now();
        if (current >= deadline) return Result::TimedOut;
        wait(std::min(interval, deadline - current));
    }
}

}  // namespace asicen::cli::loader_acquisition
