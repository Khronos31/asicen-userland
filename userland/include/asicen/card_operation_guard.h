// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <csignal>

namespace asicen {

class CardOperationGuard {
public:
    virtual ~CardOperationGuard() noexcept = default;
    virtual bool begin_card_operation(
        std::uint32_t timeout_ms,
        const volatile std::sig_atomic_t* stop_flag) noexcept = 0;
    virtual void end_card_operation() noexcept = 0;
    virtual bool begin_card_cleanup(std::uint32_t timeout_ms) noexcept = 0;
    virtual void end_card_cleanup(bool cleanup_succeeded) noexcept = 0;
    virtual void request_card_stop() noexcept = 0;
};

}  // namespace asicen
