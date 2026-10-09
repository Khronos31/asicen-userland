// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/card_only_service.h"

#include "asicen/libusb_hardware_backend.h"
#include "asicen/hardware_stream_session.h"
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

// HardwareStreamService calls its frontend shutdown while the control server
// is joining tuner and card workers concurrently. The real USB/GPIO shutdown
// is owned by the outer daemon after both workers have joined.
class DeferredHardwareShutdown final : public px4::userland::TunerServiceBackend {
public:
    explicit DeferredHardwareShutdown(LibusbW3u3Hardware& hardware) noexcept
        : hardware_(hardware) {}
    std::uint8_t receiver_count() const noexcept override {
        return hardware_.receiver_count();
    }
    bool receiver_supports(std::uint8_t receiver,
                           px4::userland::ipc::System system) const noexcept override {
        return hardware_.receiver_supports(receiver, system);
    }
    bool selects_satellite_stream_before_tune() const noexcept override {
        return hardware_.selects_satellite_stream_before_tune();
    }
    bool requires_terrestrial_lock_settle() const noexcept override {
        return hardware_.requires_terrestrial_lock_settle();
    }
    Result<void> open_receiver(std::uint8_t receiver) noexcept override {
        return hardware_.open_receiver(receiver);
    }
    Result<void> tune_terrestrial(std::uint8_t receiver, std::uint32_t frequency,
                                  std::uint32_t timeout) noexcept override {
        return hardware_.tune_terrestrial(receiver, frequency, timeout);
    }
    Result<void> tune_satellite(std::uint8_t receiver, std::uint32_t frequency,
                                std::uint32_t timeout) noexcept override {
        return hardware_.tune_satellite(receiver, frequency, timeout);
    }
    Result<bool> is_locked(std::uint8_t receiver,
                           px4::userland::ipc::System system) noexcept override {
        return hardware_.is_locked(receiver, system);
    }
    Result<void> select_satellite_slot(std::uint8_t receiver, std::uint8_t slot,
                                       std::uint32_t timeout) noexcept override {
        return hardware_.select_satellite_slot(receiver, slot, timeout);
    }
    Result<void> select_satellite_tsid(std::uint8_t receiver, std::uint16_t tsid,
                                       std::uint32_t timeout) noexcept override {
        return hardware_.select_satellite_tsid(receiver, tsid, timeout);
    }
    Result<void> close_receiver(std::uint8_t receiver) noexcept override {
        return hardware_.close_receiver(receiver);
    }
    Result<void> begin_tune_power(std::uint8_t receiver,
                                  px4::userland::ipc::System system,
                                  std::uint8_t voltage) noexcept override {
        return hardware_.begin_tune_power(receiver, system, voltage);
    }
    Result<void> commit_tune_power(std::uint8_t receiver) noexcept override {
        return hardware_.commit_tune_power(receiver);
    }
    Result<void> rollback_tune_power(std::uint8_t receiver) noexcept override {
        return hardware_.rollback_tune_power(receiver);
    }
    void mark_receiver_disconnected(std::uint8_t receiver) noexcept override {
        hardware_.mark_receiver_disconnected(receiver);
    }
    void request_stop() noexcept override { hardware_.request_stop(); }
    Result<void> shutdown() noexcept override { return Result<void>::success(); }

private:
    LibusbW3u3Hardware& hardware_;
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
    return count_;
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
    const auto* device_profile = hardware.device_profile();
    if (device_profile == nullptr || !profile_runtime_supported(*device_profile)) return 3;
    const std::uint8_t initial_receiver = device_profile->combined_isdb_ts ? 0U : 1U;
    const auto opened = hardware.open_receiver(initial_receiver);
    hardware.diagnostic_stop_flag_ = nullptr;
    if (!opened) return opened.error() == Error::UNSUPPORTED ? 3 : 70;

    W3u3CardMailboxHardware mailbox(static_cast<FrontendTransport&>(hardware), device_profile->model_id);
    W3u3CardServiceBackend card_backend(mailbox, hardware, stop_requested);
    px4::userland::CardSession raw_session(mailbox, mailbox);
    W3u3CardProtocolSession card_session(raw_session, hardware, stop_requested);
    px4::userland::CardService card(card_backend, card_session);
    CardOnlyTunerBackend tuner_backend(hardware.receiver_count());
    px4::userland::ipc::posix::PosixTunerNonceSource nonce;
    CardOnlyTime time;
    px4::userland::TunerService tuner(tuner_backend, nonce, time);
    const px4::userland::ipc::posix::EndpointConfig endpoint{
        runtime_directory != nullptr && runtime_directory[0] != '\0'
            ? runtime_directory : nullptr,
        instance, px4::userland::ipc::posix::kControlEndpointName};
    auto created = px4::userland::ipc::posix::PosixControlServer::create(
        endpoint, card, tuner, {}, true, profile::usb_present_mask(hardware.receiver_count()),
        nullptr, hardware.receiver_count(), device_profile->combined_isdb_ts);
    if (!created) {
        (void)card.shutdown();
        return created.error() == Error::BUSY ? 4 : 70;
    }

    auto server = std::move(created.value());
    std::fprintf(stderr, "asicend ready backend=asicen-libusb-card-only model=%s "
                         "tuner=unsupported endpoint=%s\n", device_profile->model_key, server->endpoint_path());
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

int run_live_card_stream_server(
    LibusbW3u3Hardware& hardware, const char* runtime_directory,
    const char* instance, const volatile std::sig_atomic_t* stop_requested) noexcept {
    // Initialize shared bridge/demod state once, then release the temporary
    // frontend lease. Card traffic does not retain receiver 1 ownership.
    hardware.diagnostic_stop_flag_ = stop_requested;
    const auto* device_profile = hardware.device_profile();
    if (device_profile == nullptr || !profile_runtime_supported(*device_profile)) return 3;
    const std::uint8_t initial_receiver = device_profile->combined_isdb_ts ? 0U : 1U;
    const auto opened = hardware.open_receiver(initial_receiver);
    hardware.diagnostic_stop_flag_ = nullptr;
    if (!opened) return opened.error() == Error::UNSUPPORTED ? 3 : 70;
    const auto closed = hardware.close_receiver(initial_receiver);
    if (!closed) return 70;

    W3u3CardMailboxHardware mailbox(static_cast<FrontendTransport&>(hardware), device_profile->model_id);
    W3u3CardServiceBackend card_backend(mailbox, hardware, stop_requested);
    px4::userland::CardSession raw_session(mailbox, mailbox);
    W3u3CardProtocolSession card_session(raw_session, hardware, stop_requested);
    px4::userland::CardService card(card_backend, card_session);
    DeferredHardwareShutdown frontend(hardware);
    ExitProcessFatal fatal;
    HardwareStreamService stream(frontend, hardware, fatal);
    px4::userland::ipc::posix::PosixTunerNonceSource nonce;
    CardOnlyTime time;
    px4::userland::TunerService tuner(stream, nonce, time,
                                      nullptr, nullptr, &stream);
    const px4::userland::ipc::posix::EndpointConfig endpoint{
        runtime_directory != nullptr && runtime_directory[0] != '\0'
            ? runtime_directory : nullptr,
        instance, px4::userland::ipc::posix::kControlEndpointName};
    auto created = px4::userland::ipc::posix::PosixControlServer::create(
        endpoint, card, tuner, {}, true, profile::usb_present_mask(hardware.receiver_count()),
        &stream, hardware.receiver_count(), device_profile->combined_isdb_ts);
    if (!created) {
        (void)card.shutdown();
        (void)stream.shutdown();
        return created.error() == Error::BUSY ? 4 : 70;
    }

    auto server = std::move(created.value());
    std::fprintf(stderr, "asicend ready backend=asicen-libusb-live-card-stream model=%s "
                         "receivers=%u endpoint=%s\n", device_profile->model_key,
                         static_cast<unsigned>(hardware.receiver_count()), server->endpoint_path());
    int result = 0;
    while (stop_requested == nullptr || *stop_requested == 0) {
        const auto polled = server->poll_once(px4::userland::Timeout{100U});
        if (!polled) {
            std::fprintf(stderr, "asicend poll: %s\n",
                         px4::userland::error_string(polled.error()));
            result = 70;
            break;
        }
    }
    // The server joins both service workers before returning. The real
    // frontend.shutdown()/GPIO restore is deliberately left to the caller.
    const auto stopped = server->shutdown();
    server.reset();
    const auto card_stopped = card.shutdown();
    if (!stopped || !card_stopped) result = 70;
    return result;
}

}  // namespace asicen
