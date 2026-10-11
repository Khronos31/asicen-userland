// SPDX-License-Identifier: GPL-2.0-only
#include "px4d_signals.h"

#include <csignal>
#include <cstdio>
#include <signal.h>

namespace {

#define PX4D_SIGNAL_CHECK(condition)                                                   \
    do {                                                                                \
        if (!(condition)) {                                                             \
            std::fprintf(stderr, "px4d signal check failed at %s:%d: %s\n",          \
                         __FILE__, __LINE__, #condition);                              \
            return false;                                                              \
        }                                                                               \
    } while (false)

class SignalRestore final {
public:
    SignalRestore() noexcept
    {
        if (::sigaction(SIGINT, nullptr, &old_int_) != 0) return;
        have_int_ = true;
        if (::sigaction(SIGTERM, nullptr, &old_term_) != 0) {
            (void)::sigaction(SIGINT, &old_int_, nullptr);
            have_int_ = false;
            return;
        }
        have_term_ = true;
        if (::sigaction(SIGHUP, nullptr, &old_hup_) != 0) {
            (void)::sigaction(SIGTERM, &old_term_, nullptr);
            (void)::sigaction(SIGINT, &old_int_, nullptr);
            have_term_ = false;
            have_int_ = false;
            return;
        }
        have_hup_ = true;
    }

    ~SignalRestore() noexcept
    {
        if (have_hup_) (void)::sigaction(SIGHUP, &old_hup_, nullptr);
        if (have_term_) (void)::sigaction(SIGTERM, &old_term_, nullptr);
        if (have_int_) (void)::sigaction(SIGINT, &old_int_, nullptr);
    }

    SignalRestore(const SignalRestore&) = delete;
    SignalRestore& operator=(const SignalRestore&) = delete;

    bool installed() const noexcept
    {
        return have_int_ && have_term_ && have_hup_;
    }

private:
    struct sigaction old_int_ {};
    struct sigaction old_term_ {};
    struct sigaction old_hup_ {};
    bool have_int_ = false;
    bool have_term_ = false;
    bool have_hup_ = false;
};

bool test_sighup_requests_stop()
{
    SignalRestore restore;
    PX4D_SIGNAL_CHECK(restore.installed());
    PX4D_SIGNAL_CHECK(!px4::userland::px4d::stop_requested());
    PX4D_SIGNAL_CHECK(px4::userland::px4d::install_signal_handlers());
    PX4D_SIGNAL_CHECK(std::raise(SIGHUP) == 0);
    PX4D_SIGNAL_CHECK(px4::userland::px4d::stop_requested());
    return true;
}

}  // namespace

bool run_px4d_signal_tests()
{
    return test_sighup_requests_stop();
}
