// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/card_only_service.h"

#include "asicen/libusb_hardware_backend.h"
#include "asicen/product_profile.h"
#include "px4/control_server.h"
#include "px4/posix_tuner_nonce.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace asicen {
namespace {

using px4::userland::Error;
using px4::userland::Result;

class CardOnlyTime final : public px4::userland::TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
};

}  // namespace

Result<void> W3u3CardServiceBackend::set_power(bool on) noexcept {
    if (on) return Result<void>::success();
    if (!guard_.begin_card_cleanup(2000U))
        return Result<void>::failure(Error::USB_IO);
    const auto result = mailbox_.shutdown_controller();
    guard_.end_card_cleanup(result.has_value());
    return result;
}

Result<void> W3u3CardServiceBackend::initialize_uart() noexcept {
    // The W3U3's mode-0f mailbox activation and 19200-baud selection are
    // performed by the source-backed CardHardware detect/reset methods.
    return Result<void>::success();
}

Result<bool> W3u3CardServiceBackend::detect_card() noexcept {
    if (!guard_.begin_card_operation(5000U, stop_requested_))
        return Result<bool>::failure(Error::NOT_READY);
    const auto result = mailbox_.detect_card();
    guard_.end_card_operation();
    return result;
}

void W3u3CardServiceBackend::request_stop() noexcept {
    guard_.request_card_stop();
}

Result<void> W3u3CardProtocolSession::initialize() noexcept {
    if (!guard_.begin_card_operation(15000U, stop_requested_))
        return Result<void>::failure(Error::NOT_READY);
    const auto result = session_.initialize();
    guard_.end_card_operation();
    return result;
}

Result<std::size_t> W3u3CardProtocolSession::transmit(
    px4::userland::ByteView apdu,
    px4::userland::MutableByteView response) noexcept {
    if (!guard_.begin_card_operation(15000U, stop_requested_))
        return Result<std::size_t>::failure(Error::NOT_READY);
    const auto result = session_.transmit(apdu, response);
    guard_.end_card_operation();
    return result;
}

bool W3u3CardProtocolSession::initialized() const noexcept {
    return session_.initialized();
}

const px4::userland::CardAtr& W3u3CardProtocolSession::atr() const noexcept {
    return session_.atr();
}

void W3u3CardProtocolSession::invalidate() noexcept {
    session_.invalidate();
}

void W3u3CardProtocolSession::request_stop() noexcept {
    guard_.request_card_stop();
}

std::uint8_t CardOnlyTunerBackend::receiver_count() const noexcept {
    return px4::userland::ipc::kReceiverCount;
}

bool CardOnlyTunerBackend::receiver_supports(
    std::uint8_t, px4::userland::ipc::System) const noexcept {
    return false;
}

Result<void> CardOnlyTunerBackend::open_receiver(std::uint8_t) noexcept {
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<void> CardOnlyTunerBackend::tune_terrestrial(
    std::uint8_t, std::uint32_t, std::uint32_t) noexcept {
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<void> CardOnlyTunerBackend::tune_satellite(
    std::uint8_t, std::uint32_t, std::uint32_t) noexcept {
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<bool> CardOnlyTunerBackend::is_locked(
    std::uint8_t, px4::userland::ipc::System) noexcept {
    return Result<bool>::failure(Error::UNSUPPORTED);
}

Result<void> CardOnlyTunerBackend::select_satellite_slot(
    std::uint8_t, std::uint8_t, std::uint32_t) noexcept {
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<void> CardOnlyTunerBackend::select_satellite_tsid(
    std::uint8_t, std::uint16_t, std::uint32_t) noexcept {
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<void> CardOnlyTunerBackend::close_receiver(std::uint8_t) noexcept {
    return Result<void>::failure(Error::UNSUPPORTED);
}

int run_card_only_server(
    LibusbW3u3Hardware& hardware, const char* runtime_directory,
    const char* instance, const volatile std::sig_atomic_t* stop_requested) noexcept {
    hardware.diagnostic_stop_flag_ = stop_requested;
    const auto opened = hardware.open_receiver(1U);
    hardware.diagnostic_stop_flag_ = nullptr;
    if (!opened) return opened.error() == Error::UNSUPPORTED ? 3 : 70;

    W3u3CardMailboxHardware mailbox(static_cast<FrontendTransport&>(hardware));
    W3u3CardServiceBackend card_backend(mailbox, hardware, stop_requested);
    px4::userland::CardSession raw_session(mailbox, mailbox);
    W3u3CardProtocolSession card_session(raw_session, hardware, stop_requested);
    px4::userland::CardService card(card_backend, card_session);
    CardOnlyTunerBackend tuner_backend;
    px4::userland::ipc::posix::PosixTunerNonceSource nonce;
    CardOnlyTime time;
    px4::userland::TunerService tuner(tuner_backend, nonce, time);
    const px4::userland::ipc::posix::EndpointConfig endpoint{
        runtime_directory != nullptr && runtime_directory[0] != '\0'
            ? runtime_directory : nullptr,
        instance, px4::userland::ipc::posix::kControlEndpointName};
    auto created = px4::userland::ipc::posix::PosixControlServer::create(
        endpoint, card, tuner, {}, true, profile::kUsbPresentMask,
        nullptr, profile::kReceiverCount, false);
    if (!created) {
        (void)card.shutdown();
        return created.error() == Error::BUSY ? 4 : 70;
    }

    auto server = std::move(created.value());
    std::fprintf(stderr, "asicend ready backend=asicen-w3u3-card-only "
                         "tuner=unsupported endpoint=%s\n", server->endpoint_path());
    while (stop_requested == nullptr || *stop_requested == 0) {
        const auto polled = server->poll_once(px4::userland::Timeout{100U});
        if (!polled) {
            (void)server->shutdown();
            server.reset();
            (void)card.shutdown();
            return 70;
        }
    }

    const auto stopped = server->shutdown();
    server.reset();
    const auto card_stopped = card.shutdown();
    return stopped && card_stopped ? 0 : 70;
}

}  // namespace asicen
