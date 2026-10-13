// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_STARTUP_READINESS_H
#define ASICEN_USERLAND_STARTUP_READINESS_H

#include "px4/error.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace asicen {

// Topology captured from a loader immediately before its successful upload.
// No USB types are exposed so the polling contract is testable offline.
struct UploadedRuntimeIdentity {
    std::size_t selected_index = 0U;
    std::uint8_t bus = 0U;
    std::vector<std::uint8_t> port_path;
    std::uint16_t vendor_id = 0U;
    std::uint16_t product_id = 0U;
};

template <typename Probe, typename Clock, typename Sleep, typename Cancel>
px4::userland::Error wait_for_uploaded_runtimes(
    const std::vector<UploadedRuntimeIdentity>& required, Probe probe,
    Clock now, Sleep sleep, Cancel cancelled)
{
    using Error = px4::userland::Error;
    using namespace std::chrono;
    if (required.empty()) return Error::INVALID_ARGUMENT;
    const auto deadline = now() + seconds(10);

    const auto check_active = [&]() {
        if (cancelled()) return Error::NOT_READY;
        if (now() >= deadline) return Error::TIMEOUT;
        return Error::OK;
    };

    for (;;) {
        const Error active = check_active();
        if (active != Error::OK) return active;
        bool all_ready = true;
        for (const UploadedRuntimeIdentity& identity : required) {
            const Error before_probe = check_active();
            if (before_probe != Error::OK) return before_probe;
            const Error result = probe(identity);
            const Error after_probe = check_active();
            if (after_probe != Error::OK) return after_probe;
            if (result == Error::NOT_FOUND) {
                all_ready = false;
            } else if (result != Error::OK) {
                return result;
            }
        }
        if (all_ready) {
            const Error before_success = check_active();
            return before_success == Error::OK ? Error::OK : before_success;
        }

        const auto current = now();
        if (current >= deadline) return Error::TIMEOUT;
        const auto remaining = duration_cast<milliseconds>(deadline - current);
        sleep(remaining < milliseconds(250) ? remaining : milliseconds(250));
    }
}

}  // namespace asicen

#endif  // ASICEN_USERLAND_STARTUP_READINESS_H
