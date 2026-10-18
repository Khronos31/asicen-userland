// SPDX-License-Identifier: GPL-2.0-only
#include "px4/card_service.h"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

using namespace px4::userland;

namespace {

class Backend final : public CardServiceBackend {
public:
    Result<void> set_power(bool on) noexcept override
    {
        record(on ? "power-on" : "power-off");
        if (on && power_on_error != Error::OK) return Result<void>::failure(power_on_error);
        if (!on && power_off_error != Error::OK) return Result<void>::failure(power_off_error);
        return Result<void>::success();
    }
    Result<void> initialize_uart() noexcept override
    {
        record("uart");
        return uart_error == Error::OK ? Result<void>::success() :
                                         Result<void>::failure(uart_error);
    }
    Result<bool> detect_card() noexcept override
    {
        record("detect");
        return detect_error == Error::OK ? Result<bool>::success(present) :
                                           Result<bool>::failure(detect_error);
    }
    std::vector<std::string> trace;
    Error power_on_error = Error::OK;
    Error power_off_error = Error::OK;
    Error uart_error = Error::OK;
    Error detect_error = Error::OK;
    bool present = true;
    std::vector<std::string>* order = nullptr;
private:
    void record(const char* event)
    {
        trace.emplace_back(event);
        if (order != nullptr) order->emplace_back(event);
    }
};

class Session final : public CardProtocolSession {
public:
    explicit Session(bool preinitialized = true)
    {
        atr_.bytes[0] = 0x3bU;
        atr_.bytes[1] = 0x91U;
        atr_.length = 2U;
        atr_.baud_rate = It930xCardBaudRate::baud_19200;
        atr_.ifsc = 41U;
        atr_.edc = CardEdc::crc;
        atr_.block_timeout_ms = 731U;
        initialized_ = preinitialized;
    }
    Result<void> initialize() noexcept override
    {
        record("initialize");
        initialized_ = initialize_error == Error::OK;
        return initialized_ ? Result<void>::success() :
                              Result<void>::failure(initialize_error);
    }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    {
        record("transmit");
        return transmit_error == Error::OK ? Result<std::size_t>::success(0U) :
                                             Result<std::size_t>::failure(transmit_error);
    }
    bool initialized() const noexcept override { return initialized_; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override
    {
        record("invalidate");
        initialized_ = false;
    }
    std::vector<std::string> trace;
    Error initialize_error = Error::OK;
    Error transmit_error = Error::OK;
    std::vector<std::string>* order = nullptr;
private:
    void record(const char* event)
    {
        trace.emplace_back(event);
        if (order != nullptr) order->emplace_back(event);
    }
    CardAtr atr_{};
    bool initialized_ = false;
};

bool equal(const CardAtr& a, const CardAtr& b)
{
    return a.bytes == b.bytes && a.length == b.length && a.baud_rate == b.baud_rate &&
           a.ifsc == b.ifsc && a.edc == b.edc && a.block_timeout_ms == b.block_timeout_ms;
}

}  // namespace

int main()
{
    Backend backend;
    Session session;
    std::vector<std::string> order;
    backend.order = &order;
    session.order = &order;
    CardService service(backend, session);
    const auto connected = service.connect(1U, ipc::ShareMode::shared);
    if (!connected || connected.value().atr.length != 0U) return 1;
    if (backend.trace != std::vector<std::string>{"power-on", "uart", "detect"}) return 2;
    if (order != std::vector<std::string>{"power-on", "uart", "detect"}) return 24;
    backend.trace.clear();
    const auto status = service.status();
    if (!status || !status.value().present || !status.value().initialized ||
        status.value().atr.length != 0U) return 3;
    if (backend.trace != std::vector<std::string>{"detect"}) return 4;
    if (order != std::vector<std::string>{"power-on", "uart", "detect", "detect"}) return 25;
    backend.trace.clear();
    backend.detect_error = Error::DISCONNECTED;
    std::array<std::uint8_t, 1U> apdu{{0U}};
    std::array<std::uint8_t, 2U> response{};
    const auto sent = service.transmit(1U, connected.value().handle,
                                       ByteView{apdu.data(), apdu.size()},
                                       MutableByteView{response.data(), response.size()});
    if (sent || sent.error() != Error::DISCONNECTED) return 5;
    if (backend.trace != std::vector<std::string>{"detect"}) return 6;
    if (order != std::vector<std::string>{"power-on", "uart", "detect", "detect",
                                          "detect", "invalidate"}) return 28;
    if (!service.shutdown()) return 7;
    if (backend.trace != std::vector<std::string>{"detect"}) return 8;
    if (order != std::vector<std::string>{"power-on", "uart", "detect", "detect",
                                          "detect", "invalidate", "invalidate"}) return 29;

    Backend fresh_backend;
    Session fresh_session(false);
    std::vector<std::string> fresh_order;
    fresh_backend.order = &fresh_order;
    fresh_session.order = &fresh_order;
    CardService fresh_service(fresh_backend, fresh_session);
    const auto fresh = fresh_service.connect(2U, ipc::ShareMode::shared);
    if (!fresh || !equal(fresh.value().atr, fresh_session.atr())) return 9;
    if (fresh_backend.trace != std::vector<std::string>{"power-on", "uart"} ||
        fresh_session.trace != std::vector<std::string>{"initialize"}) return 10;
    if (fresh_order != std::vector<std::string>{"power-on", "uart", "initialize"}) return 26;
    (void)fresh_service.shutdown();

    Backend temporary_backend;
    Session temporary_session(false);
    std::vector<std::string> temporary_order;
    temporary_backend.order = &temporary_order;
    temporary_session.order = &temporary_order;
    CardService temporary_service(temporary_backend, temporary_session);
    const auto temporary_status = temporary_service.status();
    if (!temporary_status || !temporary_status.value().present ||
        temporary_status.value().initialized || temporary_status.value().atr.length != 0U)
        return 11;
    if (temporary_backend.trace != std::vector<std::string>{
            "power-on", "uart", "detect", "power-off"}) return 12;
    if (temporary_order != std::vector<std::string>{
            "power-on", "uart", "detect", "invalidate", "power-off"}) return 27;

    Backend shared_backend;
    Session shared_session(false);
    CardService shared_service(shared_backend, shared_session);
    const auto shared_first = shared_service.connect(3U, ipc::ShareMode::shared, true);
    if (!shared_first) return 13;
    const auto shared_second = shared_service.connect(4U, ipc::ShareMode::shared, true);
    if (!shared_second) return 14;
    const auto deny_exclusive = shared_service.connect(5U, ipc::ShareMode::exclusive);
    if (deny_exclusive || deny_exclusive.error() != Error::BUSY ||
        shared_service.handle_count() != 2U) return 15;
    shared_backend.present = false;
    const auto removed = shared_service.poll_presence();
    if (!removed || !removed.value().changed || removed.value().present ||
        removed.value().reader_generation != shared_service.reader_generation()) return 16;
    shared_backend.present = true;
    const auto inserted = shared_service.poll_presence();
    if (!inserted || !inserted.value().changed || !inserted.value().present ||
        inserted.value().reader_generation != removed.value().reader_generation + 1U) return 17;
    const auto post_insertion_session_events = shared_session.trace.size();
    std::array<std::uint8_t, 1U> short_apdu{{0U}};
    std::array<std::uint8_t, 2U> short_response{};
    const auto stale = shared_service.transmit(
        3U, shared_first.value().handle, ByteView{short_apdu.data(), short_apdu.size()},
        MutableByteView{short_response.data(), short_response.size()});
    if (stale || stale.error() != Error::CARD_REMOVED ||
        shared_session.trace.size() != post_insertion_session_events) return 18;
    const auto recovered = shared_service.reconnect(
        3U, shared_first.value().handle, ipc::ShareMode::shared,
        ipc::Disposition::leave);
    if (!recovered || !equal(recovered.value(), shared_session.atr())) return 19;
    if (!shared_service.reconnect(4U, shared_second.value().handle,
                                  ipc::ShareMode::shared, ipc::Disposition::leave)) return 20;
    if (!shared_service.begin_transaction(3U, shared_first.value().handle)) return 20;
    const auto competing = shared_service.begin_transaction(4U, shared_second.value().handle);
    if (competing || competing.error() != Error::BUSY) return 21;
    if (!shared_service.end_transaction(3U, shared_first.value().handle,
                                        ipc::Disposition::leave)) return 22;
    (void)shared_service.shutdown();

    Backend stale_backend;
    Session stale_session(false);
    CardService stale_service(stale_backend, stale_session);
    const auto stale_first = stale_service.connect(7U, ipc::ShareMode::shared, true);
    const auto stale_second = stale_service.connect(8U, ipc::ShareMode::shared, true);
    if (!stale_first || !stale_second) return 30;
    stale_backend.present = false;
    if (!stale_service.poll_presence()) return 31;
    stale_backend.present = true;
    if (!stale_service.poll_presence()) return 32;
    if (!stale_service.reconnect(8U, stale_second.value().handle,
                                 ipc::ShareMode::shared, ipc::Disposition::leave)) return 33;
    if (!stale_service.begin_transaction(8U, stale_second.value().handle)) return 34;
    const auto stale_reset_events = stale_session.trace.size();
    const auto stale_reset_backend_events = stale_backend.trace.size();
    if (!stale_service.disconnect(7U, stale_first.value().handle,
                                  ipc::Disposition::reset) ||
        stale_session.trace.size() != stale_reset_events ||
        stale_backend.trace.size() != stale_reset_backend_events ||
        stale_service.handle_count() != 1U) return 35;
    if (!stale_service.end_transaction(8U, stale_second.value().handle,
                                       ipc::Disposition::leave)) return 36;
    (void)stale_service.shutdown();

    Backend precedence_backend;
    Session precedence_session(false);
    precedence_backend.power_on_error = Error::USB_IO;
    precedence_backend.power_off_error = Error::TIMEOUT;
    CardService precedence_service(precedence_backend, precedence_session);
    const auto precedence = precedence_service.connect(6U, ipc::ShareMode::shared);
    if (precedence || precedence.error() != Error::TIMEOUT ||
        precedence_backend.trace != std::vector<std::string>{"power-on", "power-off"})
        return 23;

    Backend uart_precedence_backend;
    Session uart_precedence_session(false);
    std::vector<std::string> uart_precedence_order;
    uart_precedence_backend.order = &uart_precedence_order;
    uart_precedence_session.order = &uart_precedence_order;
    uart_precedence_backend.uart_error = Error::USB_IO;
    uart_precedence_backend.power_off_error = Error::TIMEOUT;
    CardService uart_precedence_service(uart_precedence_backend, uart_precedence_session);
    const auto uart_precedence = uart_precedence_service.connect(9U, ipc::ShareMode::shared);
    if (uart_precedence || uart_precedence.error() != Error::TIMEOUT ||
        uart_precedence_backend.trace !=
            std::vector<std::string>{"power-on", "uart", "power-off"} ||
        uart_precedence_order != std::vector<std::string>{
            "power-on", "uart", "power-off", "invalidate"})
        return 37;

    Backend connect_cleanup_backend;
    Session connect_cleanup_session(false);
    std::vector<std::string> connect_cleanup_order;
    connect_cleanup_backend.order = &connect_cleanup_order;
    connect_cleanup_session.order = &connect_cleanup_order;
    connect_cleanup_session.initialize_error = Error::USB_IO;
    connect_cleanup_backend.power_off_error = Error::TIMEOUT;
    CardService connect_cleanup_service(connect_cleanup_backend, connect_cleanup_session);
    const auto connect_cleanup = connect_cleanup_service.connect(
        10U, ipc::ShareMode::shared);
    if (connect_cleanup || connect_cleanup.error() != Error::TIMEOUT ||
        connect_cleanup_backend.trace != std::vector<std::string>{
            "power-on", "uart", "power-off"} ||
        connect_cleanup_order != std::vector<std::string>{
            "power-on", "uart", "initialize", "invalidate", "invalidate", "power-off"})
        return 38;

    Backend reset_disconnect_backend;
    Session reset_disconnect_session;
    std::vector<std::string> reset_disconnect_order;
    reset_disconnect_backend.order = &reset_disconnect_order;
    reset_disconnect_session.order = &reset_disconnect_order;
    CardService reset_disconnect_service(reset_disconnect_backend,
                                         reset_disconnect_session);
    const auto reset_disconnect_handle = reset_disconnect_service.connect(
        11U, ipc::ShareMode::shared);
    if (!reset_disconnect_handle) return 39;
    reset_disconnect_session.initialize_error = Error::USB_IO;
    reset_disconnect_backend.power_off_error = Error::TIMEOUT;
    const auto reset_disconnect = reset_disconnect_service.disconnect(
        11U, reset_disconnect_handle.value().handle, ipc::Disposition::reset);
    if (reset_disconnect || reset_disconnect.error() != Error::TIMEOUT ||
        reset_disconnect_backend.trace != std::vector<std::string>{
            "power-on", "uart", "detect", "power-off"} ||
        reset_disconnect_order != std::vector<std::string>{
            "power-on", "uart", "detect", "invalidate", "initialize",
            "invalidate", "invalidate", "power-off"}) return 40;
    std::puts("card ownership characterization: PASS");
    return 0;
}
