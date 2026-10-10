// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_USERLAND_NATIVE_THREAD_H
#define ASICEN_USERLAND_NATIVE_THREAD_H

#include <pthread.h>
#include <utility>

namespace asicen {

// The same fallible pthread launch contract as the pinned PX4 control workers.
// Windows builds already use the reference's statically linked winpthreads.
class NativeThread final {
public:
    using Entry = void* (*)(void*);
    using Start = int (*)(pthread_t*, const pthread_attr_t*, Entry, void*);

    NativeThread() noexcept = default;
    ~NativeThread() noexcept { join(); }
    NativeThread(const NativeThread&) = delete;
    NativeThread& operator=(const NativeThread&) = delete;
    NativeThread(NativeThread&& other) noexcept
        : thread_(other.thread_), started_(std::exchange(other.started_, false))
    {
    }
    NativeThread& operator=(NativeThread&& other) noexcept
    {
        if (this != &other) {
            join();
            thread_ = other.thread_;
            started_ = std::exchange(other.started_, false);
        }
        return *this;
    }

    bool start(Entry entry, void* context, Start launcher = nullptr) noexcept
    {
        if (started_ || entry == nullptr) {
            return false;
        }
        const int result = launcher == nullptr ? pthread_create(&thread_, nullptr, entry, context)
                                               : launcher(&thread_, nullptr, entry, context);
        started_ = result == 0;
        return started_;
    }

    bool joinable() const noexcept { return started_; }
    void join() noexcept
    {
        if (!started_) {
            return;
        }
        (void)pthread_join(thread_, nullptr);
        started_ = false;
    }

private:
    pthread_t thread_{};
    bool started_ = false;
};

}  // namespace asicen

#endif  // ASICEN_USERLAND_NATIVE_THREAD_H
