// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_POST_GAIN_READINESS_H
#define ASICEN_POST_GAIN_READINESS_H

#include <chrono>
#include <cstring>

namespace asicen {

// Temporary, opt-in diagnostic. Not a supported product configuration.
struct PostGainDiagnosticConfig {
    bool enabled = false;
    bool valid = true;
    unsigned stable_ms = 0U;
};
inline PostGainDiagnosticConfig parse_post_gain_diagnostic(const char* value) noexcept
{
    if (value == nullptr) return {};
    if (std::strcmp(value, "0") == 0) return {true, true, 0U};
    if (std::strcmp(value, "340") == 0) return {true, true, 340U};
    return {false, false, 0U};
}

class PostGainReadiness {
public:
    using Time = std::chrono::steady_clock::time_point;
    void reset() noexcept { have_sample_ = false; since_ = {}; }
    // Call only for observations made AFTER gain has completed.
    bool observe(bool locked, Time now, unsigned stable_ms) noexcept
    {
        if (stable_ms == 0U) return locked;
        if (!locked) {
            reset();
            return false;
        }
        if (!have_sample_ || now < since_) {
            have_sample_ = true;
            since_ = now;
            return false;
        }
        return now - since_ >= std::chrono::milliseconds(stable_ms);
    }

private:
    bool have_sample_ = false;
    Time since_{};
};

} // namespace asicen
#endif

