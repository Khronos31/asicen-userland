// SPDX-License-Identifier: GPL-2.0-only
#ifndef PX4_USERLAND_WINDOWS_TUNER_NONCE_H
#define PX4_USERLAND_WINDOWS_TUNER_NONCE_H

#include "px4/tuner_service.h"

#include <array>
#include <cstdint>

namespace px4::userland::ipc::windows {

// Windows nonce source backed by BCryptGenRandom with the system-preferred
// RNG. It matches the POSIX /dev/urandom source's contract: a fixed-length
// cryptographically strong value or a fixed Error on failure.
class WindowsTunerNonceSource final : public TunerNonceSource {
public:
    WindowsTunerNonceSource() noexcept = default;

    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override;
};

}  // namespace px4::userland::ipc::windows

#endif  // PX4_USERLAND_WINDOWS_TUNER_NONCE_H
