// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/card_only_service.h"

#include "dtv/userland/host_card_t1/card_protocol_session.h"
#include "dtv/userland/host_card_t1/card_t1.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

class ProductGuardFixture final : public asicen::CardOperationGuard {
public:
    px4::userland::Result<void> begin_card_operation(
        std::uint32_t, const volatile std::sig_atomic_t*) noexcept override
    {
        ++operations;
        return px4::userland::Result<void>::success();
    }
    void end_card_operation() noexcept override { ++ended_operations; }
    px4::userland::Result<void> begin_card_cleanup(std::uint32_t) noexcept override
    {
        ++cleanups;
        return px4::userland::Result<void>::success();
    }
    void end_card_cleanup(bool) noexcept override { ++ended_cleanups; }
    void request_card_stop() noexcept override { ++stop_requests; }

    unsigned operations = 0U;
    unsigned ended_operations = 0U;
    unsigned cleanups = 0U;
    unsigned ended_cleanups = 0U;
    unsigned stop_requests = 0U;
};

class ProductCardSession final {
public:
    ProductCardSession(dtv::userland::host_card_t1::CardHardware& hardware,
                       dtv::userland::host_card_t1::CardTime& time) noexcept
        : session_(hardware, time), adapter_(session_, guard_, nullptr)
    {
    }

    dtv::userland::Result<void> initialize() noexcept { return adapter_.initialize(); }

    dtv::userland::Result<void> initialize_with_atr(
        const dtv::userland::host_card_t1::CardAtr& atr) noexcept
    {
        return adapter_.initialize_with_atr(atr);
    }

    dtv::userland::Result<std::size_t> transmit(
        dtv::userland::ByteView apdu,
        dtv::userland::MutableByteView response) noexcept
    {
        return adapter_.transmit(apdu, response);
    }

    bool initialized() const noexcept { return adapter_.initialized(); }
    const dtv::userland::host_card_t1::CardAtr& atr() const noexcept { return session_.atr(); }
    void invalidate() noexcept { adapter_.invalidate(); }
    operator dtv::userland::host_card_t1::CardSession&() noexcept { return session_; }

private:
    dtv::userland::host_card_t1::CardSession session_;
    ProductGuardFixture guard_;
    asicen::W3u3CardProtocolSession adapter_;
};

#define CardSession ProductCardSession
#include "tests/host_card_t1_tests.cpp"
#undef CardSession
