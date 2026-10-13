// SPDX-License-Identifier: GPL-2.0-only
#include "px4/card_service.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

class MockBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool on) noexcept override
    {
        power_calls.push_back(on);
        if (on && power_on_error != Error::OK) {
            return Result<void>::failure(power_on_error);
        }
        if (!on && power_off_error != Error::OK) {
            return Result<void>::failure(power_off_error);
        }
        powered = on;
        return Result<void>::success();
    }

    Result<void> initialize_uart() noexcept override
    {
        ++uart_calls;
        return uart_error == Error::OK ? Result<void>::success() :
                                         Result<void>::failure(uart_error);
    }

    Result<bool> detect_card() noexcept override
    {
        ++detect_calls;
        return detect_error == Error::OK ? Result<bool>::success(present) :
                                           Result<bool>::failure(detect_error);
    }

    std::vector<bool> power_calls;
    Error power_on_error = Error::OK;
    Error power_off_error = Error::OK;
    Error uart_error = Error::OK;
    Error detect_error = Error::OK;
    std::size_t uart_calls = 0U;
    std::size_t detect_calls = 0U;
    bool powered = false;
    bool present = true;
};

class MockSession final : public CardProtocolSession {
public:
    MockSession()
    {
        atr_.bytes[0] = 0x3bU;
        atr_.bytes[1] = 0x01U;
        atr_.bytes[2] = 0x02U;
        atr_.length = 3U;
    }

    Result<void> initialize() noexcept override
    {
        ++initialize_calls;
        if (initialize_error != Error::OK) {
            initialized_ = false;
            return Result<void>::failure(initialize_error);
        }
        initialized_ = true;
        return Result<void>::success();
    }

    Result<std::size_t> transmit(ByteView apdu,
                                 MutableByteView response) noexcept override
    {
        ++transmit_calls;
        if (transmit_error != Error::OK) {
            initialized_ = false;
            return Result<std::size_t>::failure(transmit_error);
        }
        if (!initialized_ || apdu.data == nullptr || apdu.size == 0U ||
            response.data == nullptr || response.size < 2U) {
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
        }
        response.data[0] = 0x90U;
        response.data[1] = 0x00U;
        return Result<std::size_t>::success(2U);
    }

    bool initialized() const noexcept override { return initialized_; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override
    {
        ++invalidate_calls;
        initialized_ = false;
    }

    Error initialize_error = Error::OK;
    Error transmit_error = Error::OK;
    std::size_t initialize_calls = 0U;
    std::size_t transmit_calls = 0U;
    std::size_t invalidate_calls = 0U;

private:
    CardAtr atr_{};
    bool initialized_ = false;
};

bool test_happy_path_and_one_hundred_apdus()
{
    MockBackend backend;
    MockSession session;
    CardService service(backend, session);
    const auto connected = service.connect(1U, ShareMode::shared);
    CHECK(connected && connected.value().handle != 0U);
    CHECK(service.powered() && service.handle_count() == 1U);
    CHECK(backend.power_calls.size() == 1U && backend.power_calls[0]);
    CHECK(backend.uart_calls == 1U && session.initialize_calls == 1U);

    const std::array<std::uint8_t, 5U> apdu{0x90U, 0x30U, 0x00U, 0x00U, 0x00U};
    std::array<std::uint8_t, 16U> response{};
    for (std::size_t iteration = 0U; iteration < 100U; ++iteration) {
        const auto transmitted = service.transmit(
            1U, connected.value().handle, ByteView{apdu.data(), apdu.size()},
            MutableByteView{response.data(), response.size()});
        CHECK(transmitted && transmitted.value() == 2U);
        CHECK(response[0] == 0x90U && response[1] == 0x00U);
    }
    CHECK(session.transmit_calls == 100U);

    CHECK(service.disconnect(1U, connected.value().handle, Disposition::leave));
    CHECK(!service.powered() && service.handle_count() == 0U);
    CHECK(backend.power_calls.size() == 2U && !backend.power_calls[1]);
    return true;
}

bool test_shared_exclusive_and_last_handle_power()
{
    MockBackend backend;
    MockSession session;
    CardService service(backend, session);
    const auto first = service.connect(1U, ShareMode::shared);
    const auto second = service.connect(2U, ShareMode::shared);
    CHECK(first && second && service.handle_count() == 2U);
    CHECK(backend.power_calls.size() == 1U);
    const auto denied_exclusive = service.connect(3U, ShareMode::exclusive);
    CHECK(!denied_exclusive && denied_exclusive.error() == Error::BUSY);

    CHECK(service.disconnect(1U, first.value().handle, Disposition::leave));
    CHECK(service.powered() && backend.power_calls.size() == 1U);
    CHECK(service.disconnect(2U, second.value().handle, Disposition::leave));
    CHECK(!service.powered() && backend.power_calls.size() == 2U);

    const auto exclusive = service.connect(3U, ShareMode::exclusive);
    CHECK(exclusive);
    const auto denied_shared = service.connect(4U, ShareMode::shared);
    CHECK(!denied_shared && denied_shared.error() == Error::BUSY);
    const auto wrong_client = service.reset(4U, exclusive.value().handle);
    CHECK(!wrong_client && wrong_client.error() == Error::NOT_FOUND);
    return true;
}

bool test_transaction_and_disconnect_cleanup()
{
    MockBackend backend;
    MockSession session;
    CardService service(backend, session);
    const auto first = service.connect(10U, ShareMode::shared);
    const auto second = service.connect(20U, ShareMode::shared);
    CHECK(first && second);
    CHECK(service.begin_transaction(10U, first.value().handle));
    const auto competing_begin = service.begin_transaction(20U, second.value().handle);
    CHECK(!competing_begin && competing_begin.error() == Error::BUSY);
    const std::array<std::uint8_t, 1U> apdu{0x00U};
    std::array<std::uint8_t, 8U> response{};
    const auto competing_apdu = service.transmit(
        20U, second.value().handle, ByteView{apdu.data(), apdu.size()},
        MutableByteView{response.data(), response.size()});
    CHECK(!competing_apdu && competing_apdu.error() == Error::BUSY);
    const auto competing_reset = service.reset(20U, second.value().handle);
    CHECK(!competing_reset && competing_reset.error() == Error::BUSY);
    const auto competing_reconnect = service.reconnect(
        20U, second.value().handle, ShareMode::shared, Disposition::leave);
    CHECK(!competing_reconnect && competing_reconnect.error() == Error::BUSY);
    const auto competing_disconnect = service.disconnect(
        20U, second.value().handle, Disposition::reset);
    CHECK(!competing_disconnect && competing_disconnect.error() == Error::BUSY);
    CHECK(service.handle_count() == 2U);

    const auto owner_reset = service.reset(10U, first.value().handle);
    CHECK(owner_reset);
    const auto still_busy = service.transmit(
        20U, second.value().handle, ByteView{apdu.data(), apdu.size()},
        MutableByteView{response.data(), response.size()});
    CHECK(!still_busy && still_busy.error() == Error::BUSY);
    CHECK(service.end_transaction(10U, first.value().handle, Disposition::leave));

    CHECK(service.begin_transaction(10U, first.value().handle));
    // Leaving a non-owning handle cannot mutate the card or transaction and is
    // therefore safe even while another handle owns the transaction.
    CHECK(service.disconnect(20U, second.value().handle, Disposition::leave));
    CHECK(service.handle_count() == 1U);

    CHECK(service.release_connection(10U));
    CHECK(service.handle_count() == 0U && !service.powered());

    const auto replacement = service.connect(20U, ShareMode::shared);
    CHECK(replacement);
    CHECK(service.begin_transaction(20U, replacement.value().handle));
    CHECK(service.end_transaction(20U, replacement.value().handle,
                                  Disposition::reset));
    CHECK(session.initialize_calls == 4U);
    CHECK(service.release_connection(20U));
    CHECK(!service.powered() && service.handle_count() == 0U);
    return true;
}

bool test_remove_reinsert_generation_and_invalidation()
{
    MockBackend backend;
    MockSession session;
    CardService service(backend, session);
    const auto connected = service.connect(1U, ShareMode::shared);
    const auto second = service.connect(2U, ShareMode::shared);
    CHECK(connected && second);
    CHECK(service.begin_transaction(1U, connected.value().handle));
    const std::uint64_t initial_generation = service.reader_generation();

    backend.present = false;
    const auto removed = service.poll_presence();
    CHECK(removed && removed.value().changed && !removed.value().present);
    CHECK(removed.value().reader_generation == initial_generation + 1U);
    CHECK(!session.initialized());

    session.initialize_error = Error::NO_CARD;
    const std::array<std::uint8_t, 1U> apdu{0x00U};
    std::array<std::uint8_t, 8U> response{};
    const auto absent = service.transmit(
        1U, connected.value().handle, ByteView{apdu.data(), apdu.size()},
        MutableByteView{response.data(), response.size()});
    CHECK(!absent && absent.error() == Error::NO_CARD);

    backend.present = true;
    const auto inserted = service.poll_presence();
    CHECK(inserted && inserted.value().changed && inserted.value().present);
    CHECK(inserted.value().reader_generation == initial_generation + 2U);
    session.initialize_error = Error::OK;
    CHECK(service.begin_transaction(2U, second.value().handle));
    CHECK(service.end_transaction(2U, second.value().handle, Disposition::leave));
    const auto recovered = service.transmit(
        1U, connected.value().handle, ByteView{apdu.data(), apdu.size()},
        MutableByteView{response.data(), response.size()});
    CHECK(recovered && session.initialize_calls == 3U);
    return true;
}

bool test_status_failures_and_validation()
{
    {
        MockBackend backend;
        MockSession session;
        CardService service(backend, session);
        backend.present = false;
        const auto status = service.status();
        CHECK(status && !status.value().present && !status.value().initialized);
        CHECK(backend.power_calls.size() == 2U);
        CHECK(backend.power_calls[0] && !backend.power_calls[1]);
    }
    {
        MockBackend backend;
        MockSession session;
        CardService service(backend, session);
        backend.power_on_error = Error::USB_IO;
        const auto connected = service.connect(1U, ShareMode::shared);
        CHECK(!connected && connected.error() == Error::USB_IO);
        CHECK(backend.power_calls.size() == 2U && backend.power_calls[0] &&
              !backend.power_calls[1]);
    }
    {
        MockBackend backend;
        MockSession session;
        CardService service(backend, session);
        backend.power_on_error = Error::USB_IO;
        backend.power_off_error = Error::DISCONNECTED;
        const auto connected = service.connect(1U, ShareMode::shared);
        CHECK(!connected && connected.error() == Error::DISCONNECTED);
    }
    {
        MockBackend backend;
        MockSession session;
        CardService service(backend, session);
        session.initialize_error = Error::TIMEOUT;
        backend.power_off_error = Error::USB_IO;
        const auto connected = service.connect(1U, ShareMode::shared);
        CHECK(!connected && connected.error() == Error::USB_IO);
        CHECK(service.handle_count() == 0U && !service.powered());
    }
    {
        MockBackend backend;
        MockSession session;
        CardService service(backend, session);
        const auto connected = service.connect(1U, ShareMode::shared);
        CHECK(connected);
        std::array<std::uint8_t, 8U> response{};
        const auto empty = service.transmit(
            1U, connected.value().handle, ByteView{nullptr, 0U},
            MutableByteView{response.data(), response.size()});
        CHECK(!empty && empty.error() == Error::INVALID_ARGUMENT);
        session.transmit_error = Error::TIMEOUT;
        const std::array<std::uint8_t, 1U> apdu{0x00U};
        const auto timeout = service.transmit(
            1U, connected.value().handle, ByteView{apdu.data(), apdu.size()},
            MutableByteView{response.data(), response.size()});
        CHECK(!timeout && timeout.error() == Error::TIMEOUT);
        CHECK(!session.initialized());
    }
    return true;
}

bool test_generation_bound_handles_and_explicit_recovery()
{
    MockBackend backend;
    MockSession session;
    CardService service(backend, session);
    const auto first = service.connect(1U, ShareMode::shared, true);
    const auto second = service.connect(2U, ShareMode::shared, true);
    const auto legacy = service.connect(3U, ShareMode::shared);
    CHECK(first && second && legacy);
    CHECK(service.begin_transaction(1U, first.value().handle));
    backend.present = false;
    CHECK(service.poll_presence());
    backend.present = true;
    CHECK(service.poll_presence());
    const auto initializations = session.initialize_calls;
    const auto detects = backend.detect_calls;
    const std::array<std::uint8_t, 1U> apdu{0x00U};
    std::array<std::uint8_t, 8U> response{};
    const auto send = [&](CardClientId client, std::uint64_t handle) {
        return service.transmit(client, handle, ByteView{apdu.data(), apdu.size()},
                                MutableByteView{response.data(), response.size()});
    };
    const auto stale = send(1U, first.value().handle);
    const auto begin = service.begin_transaction(2U, second.value().handle);
    const auto end = service.end_transaction(1U, first.value().handle, Disposition::reset);
    CHECK(!stale && stale.error() == Error::CARD_REMOVED);
    CHECK(!begin && begin.error() == Error::CARD_REMOVED);
    CHECK(!end && end.error() == Error::CARD_REMOVED);
    CHECK(session.initialize_calls == initializations && session.transmit_calls == 0U &&
          backend.detect_calls == detects);

    // Existing consumers still transparently recover. Their initialization
    // must not silently rebind either generation-bound handle.
    CHECK(send(3U, legacy.value().handle));
    CHECK(!send(1U, first.value().handle));
    const auto wrong_owner = service.reset(2U, first.value().handle);
    CHECK(!wrong_owner && wrong_owner.error() == Error::NOT_FOUND);
    CHECK(service.reconnect(1U, first.value().handle, ShareMode::shared,
                            Disposition::leave));
    CHECK(send(1U, first.value().handle));
    CHECK(!send(2U, second.value().handle));
    CHECK(service.reset(2U, second.value().handle));
    CHECK(send(2U, second.value().handle));

    // A failed explicit reset must keep the handle stale. A subsequent
    // explicit reconnect/reset can rebind only the requesting handle.
    backend.present = false;
    CHECK(service.poll_presence());
    session.initialize_error = Error::NO_CARD;
    CHECK(!service.reset(1U, first.value().handle));
    backend.present = true;
    CHECK(service.poll_presence());
    session.initialize_error = Error::OK;
    CHECK(!send(1U, first.value().handle));
    CHECK(service.reconnect(1U, first.value().handle, ShareMode::shared,
                            Disposition::reset));
    CHECK(service.begin_transaction(1U, first.value().handle));
    const auto before_cleanup = session.initialize_calls;
    CHECK(service.disconnect(2U, second.value().handle, Disposition::reset));
    CHECK(session.initialize_calls == before_cleanup && service.handle_count() == 2U);
    const auto busy = send(3U, legacy.value().handle);
    CHECK(!busy && busy.error() == Error::BUSY);
    CHECK(service.end_transaction(1U, first.value().handle, Disposition::leave));
    CHECK(service.release_connection(1U));
    CHECK(service.release_connection(3U));
    CHECK(service.handle_count() == 0U && !service.powered());
    return true;
}

bool test_generation_bound_observation_paths()
{
    const std::array<std::uint8_t, 1U> apdu{0x00U};
    std::array<std::uint8_t, 8U> response{};
    // Removal may be observed by readiness detection, status, backend error,
    // protocol error, or disconnected USB. None permits later implicit reuse.
    for (unsigned mode = 0U; mode < 9U; ++mode) {
        MockBackend backend;
        MockSession session;
        CardService service(backend, session);
        const auto card = service.connect(1U, ShareMode::shared, true);
        CHECK(card);
        const auto send = [&]() {
            return service.transmit(1U, card.value().handle,
                                    ByteView{apdu.data(), apdu.size()},
                                    MutableByteView{response.data(), response.size()});
        };
        if (mode == 0U || mode == 1U) {
            backend.present = false;
            if (mode == 0U) {
                CHECK(!send());
                CHECK(session.transmit_calls == 0U && session.initialize_calls == 1U);
            } else {
                CHECK(service.status());
            }
        } else if (mode == 2U || mode == 3U) {
            backend.detect_error = mode == 2U ? Error::CARD_REMOVED : Error::DISCONNECTED;
            CHECK(!service.poll_presence());
        } else if (mode == 4U || mode == 5U) {
            session.transmit_error = mode == 4U ? Error::NO_CARD : Error::CARD_REMOVED;
            CHECK(!send());
            CHECK(session.transmit_calls == 1U);
        } else {
            // A timeout itself retains legacy retry behavior, but the next
            // readiness check detects absence before protocol initialization.
            session.transmit_error = Error::TIMEOUT;
            CHECK(!send());
            if (mode == 6U) backend.present = false;
            else session.initialize_error = mode == 7U ? Error::NO_CARD : Error::CARD_REMOVED;
            CHECK(!send());
            CHECK(session.initialize_calls == (mode == 6U ? 1U : 2U));
        }
        backend.present = true;
        backend.detect_error = Error::OK;
        session.transmit_error = Error::OK;
        session.initialize_error = Error::OK;
        const auto before_init = session.initialize_calls;
        const auto before_send = session.transmit_calls;
        const auto before_detect = backend.detect_calls;
        const auto stale = send();
        CHECK(!stale && stale.error() == Error::CARD_REMOVED);
        const auto transaction = service.begin_transaction(1U, card.value().handle);
        CHECK(!transaction && transaction.error() == Error::CARD_REMOVED);
        CHECK(session.initialize_calls == before_init && session.transmit_calls == before_send &&
              backend.detect_calls == before_detect);
        CHECK(service.disconnect(1U, card.value().handle, Disposition::leave));
        CHECK(!service.has_handles());
    }
    return true;
}

bool test_owned_handle_status_is_atomic_and_read_only()
{
    MockBackend backend;
    MockSession session;
    CardService service(backend, session);
    const auto bound = service.connect(1U, ShareMode::shared, true);
    const auto legacy = service.connect(2U, ShareMode::shared);
    CHECK(bound && legacy);
    const auto initial = service.handle_status(1U, bound.value().handle);
    CHECK(initial && initial.value().present && initial.value().initialized &&
          initial.value().atr.length == 3U && session.initialize_calls == 1U &&
          session.transmit_calls == 0U);
    const auto detects = backend.detect_calls;
    const auto foreign = service.handle_status(2U, bound.value().handle);
    CHECK(!foreign && foreign.error() == Error::NOT_FOUND && backend.detect_calls == detects);
    const std::array<std::uint8_t, 1U> apdu{0x00U};
    std::array<std::uint8_t, 8U> response{};
    const auto legacy_send = [&]() {
        return service.transmit(2U, legacy.value().handle, ByteView{apdu.data(), apdu.size()},
                                MutableByteView{response.data(), response.size()});
    };
    // A protocol failure may invalidate initialization without card loss. A
    // snapshot must report NOT_READY, never initialize it or send a probe.
    session.transmit_error = Error::TIMEOUT;
    CHECK(!legacy_send());
    const auto uninitialized = service.handle_status(1U, bound.value().handle);
    CHECK(!uninitialized && uninitialized.error() == Error::NOT_READY);
    CHECK(session.initialize_calls == 1U && session.transmit_calls == 1U);
    session.transmit_error = Error::OK;
    CHECK(legacy_send());

    // Reproduce a loss that never advances the reader-wide generation. The
    // legacy client recovers; an owned-handle snapshot still rejects the old
    // bound handle before any detection, initialization, or APDU.
    session.transmit_error = Error::CARD_REMOVED;
    CHECK(!legacy_send());
    session.transmit_error = Error::OK;
    CHECK(legacy_send());
    const auto global = service.status();
    CHECK(global && global.value().initialized &&
          global.value().reader_generation == initial.value().reader_generation);
    const auto initializations = session.initialize_calls;
    const auto transmits = session.transmit_calls;
    const auto before_stale_detect = backend.detect_calls;
    const auto stale = service.handle_status(1U, bound.value().handle);
    CHECK(!stale && stale.error() == Error::CARD_REMOVED);
    CHECK(backend.detect_calls == before_stale_detect &&
          session.initialize_calls == initializations && session.transmit_calls == transmits);
    CHECK(service.reset(1U, bound.value().handle));
    CHECK(service.handle_status(1U, bound.value().handle));

    // A loss detected within the snapshot also invalidates that very result.
    backend.present = false;
    const auto during = service.handle_status(1U, bound.value().handle);
    CHECK(!during && during.error() == Error::CARD_REMOVED);
    CHECK(session.initialize_calls == initializations + 1U && session.transmit_calls == transmits);
    CHECK(service.disconnect(1U, bound.value().handle, Disposition::leave));
    const auto closed = service.handle_status(1U, bound.value().handle);
    CHECK(!closed && closed.error() == Error::NOT_FOUND);
    return true;
}

bool test_usb_disconnect_abandons_without_power_transfer()
{
    {
        MockBackend backend;
        MockSession session;
        CardService service(backend, session);
        backend.power_on_error = Error::DISCONNECTED;
        const auto connected = service.connect(1U, ShareMode::shared);
        CHECK(!connected && connected.error() == Error::DISCONNECTED);
        CHECK(backend.power_calls.size() == 1U && backend.power_calls[0]);
        CHECK(!service.powered());
    }
    {
        MockBackend backend;
        MockSession session;
        CardService service(backend, session);
        const auto connected = service.connect(1U, ShareMode::shared);
        CHECK(connected && backend.power_calls.size() == 1U);
        backend.detect_error = Error::DISCONNECTED;
        const auto polled = service.poll_presence();
        CHECK(!polled && polled.error() == Error::DISCONNECTED);
        CHECK(!service.powered() && !session.initialized());
        CHECK(backend.power_calls.size() == 1U);
        CHECK(service.shutdown());
        CHECK(backend.power_calls.size() == 1U);
    }
    return true;
}

bool test_status_does_not_consume_presence_event()
{
    MockBackend backend;
    MockSession session;
    CardService service(backend, session);
    const auto connected = service.connect(1U, ShareMode::shared);
    CHECK(connected);
    (void)service.poll_presence();

    backend.present = false;
    const auto status = service.status();
    CHECK(status && !status.value().present);
    const auto event = service.poll_presence();
    CHECK(event && event.value().changed && !event.value().present &&
          event.value().reader_generation == service.reader_generation());
    const auto no_duplicate = service.poll_presence();
    CHECK(no_duplicate && !no_duplicate.value().changed);
    return true;
}

}  // namespace

bool run_card_service_tests()
{
    return test_happy_path_and_one_hundred_apdus() &&
           test_shared_exclusive_and_last_handle_power() &&
           test_transaction_and_disconnect_cleanup() &&
           test_remove_reinsert_generation_and_invalidation() &&
           test_generation_bound_handles_and_explicit_recovery() &&
           test_generation_bound_observation_paths() &&
           test_owned_handle_status_is_atomic_and_read_only() &&
           test_status_failures_and_validation() &&
           test_usb_disconnect_abandons_without_power_transfer() &&
           test_status_does_not_consume_presence_event();
}
