// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/card_only_service.h"
#include "asicen/control_context_builder.h"

#include "asicen/libusb_hardware_backend.h"
#include "asicen/hardware_stream_session.h"
#include "asicen/product_profile.h"
#include "px4/control_server.h"
#if defined(_WIN32)
#include "px4/windows_tuner_nonce.h"
#else
#include "px4/posix_tuner_nonce.h"
#endif

#include <chrono>
#include <array>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>
#include <utility>

namespace asicen {
namespace {

using px4::userland::Error;
using px4::userland::Result;

int exit_status(Error error) noexcept
{
    switch (error) {
    case Error::OK:
        return 0;
    case Error::INVALID_ARGUMENT:
        return 2;
    case Error::NOT_FOUND:
    case Error::NOT_READY:
    case Error::NO_CARD:
    case Error::UNSUPPORTED:
        return 3;
    case Error::BUSY:
        return 4;
    case Error::TIMEOUT:
        return 5;
    case Error::VERSION_MISMATCH:
    case Error::PROTOCOL_ERROR:
        return 6;
    case Error::USB_IO:
    case Error::DISCONNECTED:
        return 7;
    case Error::SLOW_CONSUMER:
        return 8;
    case Error::CARD_REMOVED:
    case Error::BUFFER_TOO_SMALL:
        return 9;
    case Error::FIRMWARE_REJECTED:
        return 10;
    case Error::RESOURCE_LIMIT:
        return 70;
    case Error::INTERNAL:
        return 70;
    }
    return 70;
}
#if defined(_WIN32)
using PlatformTunerNonceSource = px4::userland::ipc::windows::WindowsTunerNonceSource;
#else
using PlatformTunerNonceSource = px4::userland::ipc::posix::PosixTunerNonceSource;
#endif

namespace wire = dtv::userland::wire;

bool build_context_from_claims(const LibusbW3u3Hardware& hardware,
                               const px4::userland::TunerService& tuner,
                               const char* instance, const wire::Epoch& epoch,
                               px4::userland::ipc::posix::ControlContext& out,
                               bool tuner_enabled = true) noexcept
{
    const auto* profile = hardware.device_profile();
    std::array<ClaimedUsbFunctionObservation, 2U> observations{};
    std::uint8_t count = 0U;
    if (profile == nullptr || !hardware.claimed_function_observations(observations, count))
        return false;
    return build_asicen_control_context(*profile, observations.data(), count,
        hardware.receiver_count(), tuner, instance, epoch, out, tuner_enabled);
}

class CardOnlyTime final : public px4::userland::TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
};

// HardwareStreamService calls its frontend shutdown while the control server
// is joining tuner and card workers concurrently. The real USB/GPIO shutdown
// is owned by the outer daemon after both workers have joined.
class DeferredHardwareShutdown final : public px4::userland::TunerServiceBackend {
public:
    explicit DeferredHardwareShutdown(LibusbW3u3Hardware& hardware) noexcept : hardware_(hardware)
    {
    }
    std::uint8_t receiver_count() const noexcept override { return hardware_.receiver_count(); }
    bool receiver_supports(std::uint8_t receiver,
                           px4::userland::ipc::System system) const noexcept override
    {
        return hardware_.receiver_supports(receiver, system);
    }
    bool selects_satellite_stream_before_tune() const noexcept override
    {
        return hardware_.selects_satellite_stream_before_tune();
    }
    bool requires_terrestrial_lock_settle() const noexcept override
    {
        return hardware_.requires_terrestrial_lock_settle();
    }
    px4::userland::TunerStrictProfile strict_profile(
        std::uint8_t receiver, px4::userland::ipc::System system) const noexcept override
    {
        return hardware_.strict_profile(receiver, system);
    }
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        return hardware_.open_receiver(receiver);
    }
    Result<void> tune_terrestrial(std::uint8_t receiver, std::uint32_t frequency,
                                  std::uint32_t timeout) noexcept override
    {
        return hardware_.tune_terrestrial(receiver, frequency, timeout);
    }
    Result<void> tune_satellite(std::uint8_t receiver, std::uint32_t frequency,
                                std::uint32_t timeout) noexcept override
    {
        return hardware_.tune_satellite(receiver, frequency, timeout);
    }
    Result<px4::userland::TunerHzProgramming> tune_terrestrial_hz(
        std::uint8_t receiver, std::uint64_t frequency,
        std::uint32_t timeout) noexcept override
    {
        return hardware_.tune_terrestrial_hz(receiver, frequency, timeout);
    }
    Result<px4::userland::TunerHzProgramming> tune_satellite_hz(
        std::uint8_t receiver, std::uint64_t frequency,
        std::uint32_t timeout) noexcept override
    {
        return hardware_.tune_satellite_hz(receiver, frequency, timeout);
    }
    Result<bool> is_locked(std::uint8_t receiver,
                           px4::userland::ipc::System system) noexcept override
    {
        return hardware_.is_locked(receiver, system);
    }
    Result<void> select_satellite_slot(std::uint8_t receiver, std::uint8_t slot,
                                       std::uint32_t timeout) noexcept override
    {
        return hardware_.select_satellite_slot(receiver, slot, timeout);
    }
    Result<void> select_satellite_tsid(std::uint8_t receiver, std::uint16_t tsid,
                                       std::uint32_t timeout) noexcept override
    {
        return hardware_.select_satellite_tsid(receiver, tsid, timeout);
    }
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    {
        return hardware_.close_receiver(receiver);
    }
    Result<void> begin_tune_power(std::uint8_t receiver, px4::userland::ipc::System system,
                                  std::uint8_t voltage) noexcept override
    {
        return hardware_.begin_tune_power(receiver, system, voltage);
    }
    Result<void> commit_tune_power(std::uint8_t receiver) noexcept override
    {
        return hardware_.commit_tune_power(receiver);
    }
    Result<void> rollback_tune_power(std::uint8_t receiver) noexcept override
    {
        return hardware_.rollback_tune_power(receiver);
    }
    void mark_receiver_disconnected(std::uint8_t receiver) noexcept override
    {
        hardware_.mark_receiver_disconnected(receiver);
    }
    void request_stop() noexcept override { hardware_.request_stop(); }
    Result<void> shutdown() noexcept override { return Result<void>::success(); }

private:
    LibusbW3u3Hardware& hardware_;
};

}  // namespace

class AsicenEnclosureFamily final : public px4::userland::ipc::posix::EnclosureFamily {
public:
    AsicenEnclosureFamily(std::unique_ptr<LibusbContext> context_owner,
                          std::unique_ptr<LibusbW3u3Hardware> hardware,
                          const volatile std::sig_atomic_t* stop_requested) noexcept
        : context_owner_(std::move(context_owner)), hardware_(std::move(hardware)),
          mailbox_(static_cast<FrontendTransport&>(*hardware_),
                   hardware_->device_profile()->model_id),
          card_backend_(mailbox_, *hardware_, stop_requested),
          raw_session_(mailbox_, mailbox_),
          card_protocol_(raw_session_, *hardware_, stop_requested),
          frontend_(*hardware_), stream_(frontend_, *hardware_)
    {
    }

    ~AsicenEnclosureFamily() noexcept override { (void)shutdown(); }

    px4::userland::Result<void> prepare_initial_receiver(
        const volatile std::sig_atomic_t* stop_requested) noexcept
    {
        const auto* profile = hardware_->device_profile();
        const std::uint8_t receiver = profile->combined_isdb_ts ? 0U : 1U;
        hardware_->diagnostic_stop_flag_ = stop_requested;
        const auto opened = hardware_->open_receiver(receiver);
        hardware_->diagnostic_stop_flag_ = nullptr;
        if (!opened) return opened;
        return hardware_->close_receiver(receiver);
    }

    px4::userland::Result<void> shutdown() noexcept override
    {
        if (shutdown_attempted_) return shutdown_result_;
        shutdown_attempted_ = true;
        shutdown_result_ = hardware_->shutdown();
        const auto released = hardware_->release();
        if (shutdown_result_ && !released) shutdown_result_ = released;
        return shutdown_result_;
    }

    LibusbW3u3Hardware& hardware() noexcept { return *hardware_; }
    px4::userland::TunerServiceBackend& tuner_backend() noexcept override { return stream_; }
    px4::userland::CardServiceBackend& card_backend() noexcept override { return card_backend_; }
    px4::userland::CardProtocolSession& card_protocol() noexcept override
    {
        return card_protocol_;
    }
    px4::userland::TunerServiceTime& time() noexcept override { return time_; }
    px4::userland::TunerStreamControl* stream_control() noexcept override { return &stream_; }

private:
    std::unique_ptr<LibusbContext> context_owner_;
    std::unique_ptr<LibusbW3u3Hardware> hardware_;
    W3u3CardMailboxHardware mailbox_;
    W3u3CardServiceBackend card_backend_;
    px4::userland::CardSession raw_session_;
    W3u3CardProtocolSession card_protocol_;
    DeferredHardwareShutdown frontend_;
    HardwareStreamService stream_;
    CardOnlyTime time_;
    bool shutdown_attempted_ = false;
    px4::userland::Result<void> shutdown_result_ = px4::userland::Result<void>::success();
};

Result<void> W3u3CardServiceBackend::set_power(bool on) noexcept
{
    if (on) {
        return Result<void>::success();
    }
    const auto entered = guard_.begin_card_cleanup(2000U);
    if (!entered) {
        return entered;
    }
    const auto result = mailbox_.shutdown_controller();
    guard_.end_card_cleanup(result.has_value());
    return result;
}

Result<void> W3u3CardServiceBackend::initialize_uart() noexcept
{
    // The W3U3's mode-0f mailbox activation and 19200-baud selection are
    // performed by the source-backed CardHardware detect/reset methods.
    return Result<void>::success();
}

Result<bool> W3u3CardServiceBackend::detect_card() noexcept
{
    const auto entered = guard_.begin_card_operation(5000U, stop_requested_);
    if (!entered) {
        return Result<bool>::failure(entered.error());
    }
    const auto result = mailbox_.detect_card();
    guard_.end_card_operation();
    return result;
}

void W3u3CardServiceBackend::request_stop() noexcept
{
    guard_.request_card_stop();
}

Result<void> W3u3CardProtocolSession::initialize() noexcept
{
    const auto entered = guard_.begin_card_operation(15000U, stop_requested_);
    if (!entered) {
        return entered;
    }
    const auto result = session_.initialize();
    guard_.end_card_operation();
    return result;
}

Result<std::size_t>
W3u3CardProtocolSession::transmit(px4::userland::ByteView apdu,
                                  px4::userland::MutableByteView response) noexcept
{
    const auto entered = guard_.begin_card_operation(15000U, stop_requested_);
    if (!entered) {
        return Result<std::size_t>::failure(entered.error());
    }
    const auto result = session_.transmit(apdu, response);
    guard_.end_card_operation();
    return result;
}

bool W3u3CardProtocolSession::initialized() const noexcept
{
    return session_.initialized();
}

const px4::userland::CardAtr& W3u3CardProtocolSession::atr() const noexcept
{
    return session_.atr();
}

void W3u3CardProtocolSession::invalidate() noexcept
{
    session_.invalidate();
}

void W3u3CardProtocolSession::request_stop() noexcept
{
    guard_.request_card_stop();
}

std::uint8_t CardOnlyTunerBackend::receiver_count() const noexcept
{
    return count_;
}

bool CardOnlyTunerBackend::receiver_supports(std::uint8_t,
                                             px4::userland::ipc::System) const noexcept
{
    return false;
}

Result<void> CardOnlyTunerBackend::open_receiver(std::uint8_t) noexcept
{
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<void> CardOnlyTunerBackend::tune_terrestrial(std::uint8_t, std::uint32_t,
                                                    std::uint32_t) noexcept
{
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<void> CardOnlyTunerBackend::tune_satellite(std::uint8_t, std::uint32_t,
                                                  std::uint32_t) noexcept
{
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<bool> CardOnlyTunerBackend::is_locked(std::uint8_t, px4::userland::ipc::System) noexcept
{
    return Result<bool>::failure(Error::UNSUPPORTED);
}

Result<void> CardOnlyTunerBackend::select_satellite_slot(std::uint8_t, std::uint8_t,
                                                         std::uint32_t) noexcept
{
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<void> CardOnlyTunerBackend::select_satellite_tsid(std::uint8_t, std::uint16_t,
                                                         std::uint32_t) noexcept
{
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<void> CardOnlyTunerBackend::close_receiver(std::uint8_t) noexcept
{
    return Result<void>::failure(Error::UNSUPPORTED);
}

int run_card_only_server(LibusbW3u3Hardware& hardware, const char* runtime_directory,
                         const char* instance, bool group,
                         const volatile std::sig_atomic_t* stop_requested,
                         bool (*stop_callback)() noexcept) noexcept
{
    hardware.diagnostic_stop_flag_ = stop_requested;
    const auto* device_profile = hardware.device_profile();
    if (device_profile == nullptr || !profile_runtime_supported(*device_profile)) {
        return 3;
    }
    const std::uint8_t initial_receiver = device_profile->combined_isdb_ts ? 0U : 1U;
    const auto opened = hardware.open_receiver(initial_receiver);
    hardware.diagnostic_stop_flag_ = nullptr;
    if (!opened) {
        return exit_status(opened.error());
    }

    W3u3CardMailboxHardware mailbox(static_cast<FrontendTransport&>(hardware),
                                    device_profile->model_id);
    W3u3CardServiceBackend card_backend(mailbox, hardware, stop_requested);
    px4::userland::CardSession raw_session(mailbox, mailbox);
    W3u3CardProtocolSession card_session(raw_session, hardware, stop_requested);
    px4::userland::CardService card(card_backend, card_session);
    CardOnlyTunerBackend tuner_backend(hardware.receiver_count());
    PlatformTunerNonceSource nonce;
    CardOnlyTime time;
    px4::userland::TunerService tuner(tuner_backend, nonce, time);
    const px4::userland::ipc::posix::EndpointConfig endpoint{
        runtime_directory != nullptr && runtime_directory[0] != '\0' ? runtime_directory : nullptr,
        instance, px4::userland::ipc::posix::kControlEndpointName,
        group ? px4::userland::ipc::posix::EndpointAccess::shared_group
              : px4::userland::ipc::posix::EndpointAccess::private_user};
    const auto epoch = nonce.generate();
    if (!epoch) {
        (void)card.shutdown();
        return exit_status(epoch.error());
    }
    px4::userland::ipc::posix::ControlContext context;
    if (!build_context_from_claims(hardware, tuner,
                                      endpoint.instance != nullptr ? endpoint.instance : "",
                                      epoch.value(), context, false)) {
        (void)card.shutdown();
        return 3;
    }
    auto created = px4::userland::ipc::posix::PosixControlServer::create(
        endpoint, card, tuner, context);
    if (!created) {
        (void)card.shutdown();
        return exit_status(created.error());
    }

    auto server = std::move(created.value());
    std::fprintf(stderr,
                 "asicend ready backend=asicen-libusb-card-only model=%s "
#if defined(_WIN32)
                 "tuner=unsupported endpoint=%ls\n",
#else
                 "tuner=unsupported endpoint=%s\n",
#endif
                 device_profile->model_key, server->endpoint_path());
    Error loop_error = Error::OK;
    while ((stop_requested == nullptr || *stop_requested == 0) &&
           (stop_callback == nullptr || !stop_callback())) {
        const auto polled = server->poll_once(px4::userland::Timeout{100U});
        if (!polled) {
            loop_error = polled.error();
            break;
        }
    }

    const auto stopped = server->shutdown();
    server.reset();
    const auto card_stopped = card.shutdown();
    if (!stopped && loop_error == Error::OK) {
        loop_error = stopped.error();
    }
    if (!card_stopped && loop_error == Error::OK) {
        loop_error = card_stopped.error();
    }
    return exit_status(loop_error);
}

int run_live_card_stream_server(LibusbW3u3Hardware& hardware, const char* runtime_directory,
                                const char* instance, bool group,
                                const volatile std::sig_atomic_t* stop_requested,
                                bool (*stop_callback)() noexcept) noexcept
{
    // Initialize shared bridge/demod state once, then release the temporary
    // frontend lease. Card traffic does not retain receiver 1 ownership.
    hardware.diagnostic_stop_flag_ = stop_requested;
    const auto* device_profile = hardware.device_profile();
    if (device_profile == nullptr || !profile_runtime_supported(*device_profile)) {
        return 3;
    }
    const std::uint8_t initial_receiver = device_profile->combined_isdb_ts ? 0U : 1U;
    const auto opened = hardware.open_receiver(initial_receiver);
    hardware.diagnostic_stop_flag_ = nullptr;
    if (!opened) {
        return exit_status(opened.error());
    }
    const auto closed = hardware.close_receiver(initial_receiver);
    if (!closed) {
        return exit_status(closed.error());
    }

    W3u3CardMailboxHardware mailbox(static_cast<FrontendTransport&>(hardware),
                                    device_profile->model_id);
    W3u3CardServiceBackend card_backend(mailbox, hardware, stop_requested);
    px4::userland::CardSession raw_session(mailbox, mailbox);
    W3u3CardProtocolSession card_session(raw_session, hardware, stop_requested);
    px4::userland::CardService card(card_backend, card_session);
    DeferredHardwareShutdown frontend(hardware);

    HardwareStreamService stream(frontend, hardware);
    PlatformTunerNonceSource nonce;
    CardOnlyTime time;
    px4::userland::TunerService tuner(stream, nonce, time, nullptr, nullptr, &stream);
    // The real receiver daemon opts into R2 strict admission once, before its
    // workers start, from the audited profile/model capability metadata.
    const auto strict_capabilities = tuner.configure_strict_capabilities();
    if (!strict_capabilities) {
        std::fprintf(stderr, "strict capabilities: %s\n",
                     px4::userland::error_string(strict_capabilities.error()));
        (void)card.shutdown();
        (void)stream.shutdown();
        return exit_status(strict_capabilities.error());
    }
    const px4::userland::ipc::posix::EndpointConfig endpoint{
        runtime_directory != nullptr && runtime_directory[0] != '\0' ? runtime_directory : nullptr,
        instance, px4::userland::ipc::posix::kControlEndpointName,
        group ? px4::userland::ipc::posix::EndpointAccess::shared_group
              : px4::userland::ipc::posix::EndpointAccess::private_user};
    const auto epoch = nonce.generate();
    if (!epoch) {
        (void)card.shutdown();
        (void)stream.shutdown();
        return exit_status(epoch.error());
    }
    px4::userland::ipc::posix::ControlContext context;
    if (!build_context_from_claims(hardware, tuner,
                                      endpoint.instance != nullptr ? endpoint.instance : "",
                                      epoch.value(), context)) {
        (void)card.shutdown();
        (void)stream.shutdown();
        return 3;
    }
    auto created = px4::userland::ipc::posix::PosixControlServer::create(
        endpoint, card, tuner, context, &stream);
    if (!created) {
        (void)card.shutdown();
        (void)stream.shutdown();
        return exit_status(created.error());
    }

    auto server = std::move(created.value());
    std::fprintf(stderr,
                 "asicend ready backend=asicen-libusb-live-card-stream model=%s "
#if defined(_WIN32)
                 "receivers=%u endpoint=%ls\n",
#else
                 "receivers=%u endpoint=%s\n",
#endif
                 device_profile->model_key, static_cast<unsigned>(hardware.receiver_count()),
                 server->endpoint_path());
    Error loop_error = Error::OK;
    while ((stop_requested == nullptr || *stop_requested == 0) &&
           (stop_callback == nullptr || !stop_callback())) {
        const auto polled = server->poll_once(px4::userland::Timeout{100U});
        if (!polled) {
            std::fprintf(stderr, "asicend poll: %s\n", px4::userland::error_string(polled.error()));
            loop_error = polled.error();
            break;
        }
    }
    // The server joins both service workers before returning. The real
    // frontend.shutdown()/GPIO restore is deliberately left to the caller.
    const auto stopped = server->shutdown();
    server.reset();
    const auto card_stopped = card.shutdown();
    if (!stopped && loop_error == Error::OK) {
        loop_error = stopped.error();
    }
    if (!card_stopped && loop_error == Error::OK) {
        loop_error = card_stopped.error();
    }
    return exit_status(loop_error);
}

Result<std::unique_ptr<px4::userland::ipc::posix::EnclosureContext>>
create_live_card_stream_context(
    std::unique_ptr<LibusbContext> context_owner,
    std::unique_ptr<LibusbW3u3Hardware> hardware,
    px4::userland::ipc::posix::SerialEndpointLease lease,
    const px4::userland::ipc::posix::EnclosurePublication& publication,
    const volatile std::sig_atomic_t* stop_requested) noexcept
{
    using Owner = px4::userland::ipc::posix::EnclosureContext;
    using Family = px4::userland::ipc::posix::EnclosureFamily;
    if (!hardware)
        return Result<std::unique_ptr<Owner>>::failure(Error::INVALID_ARGUMENT);
    if (!context_owner || !lease.valid() || hardware->device_profile() == nullptr) {
        (void)hardware->shutdown();
        (void)hardware->release();
        return Result<std::unique_ptr<Owner>>::failure(Error::INVALID_ARGUMENT);
    }

    const auto* device_profile = hardware->device_profile();
    if (!profile_runtime_supported(*device_profile)) {
        (void)hardware->shutdown();
        (void)hardware->release();
        return Result<std::unique_ptr<Owner>>::failure(Error::UNSUPPORTED);
    }
    std::unique_ptr<AsicenEnclosureFamily> asicen_family(new (std::nothrow)
        AsicenEnclosureFamily(std::move(context_owner), std::move(hardware), stop_requested));
    if (!asicen_family) {
        if (hardware) {
            (void)hardware->shutdown();
            (void)hardware->release();
        }
        return Result<std::unique_ptr<Owner>>::failure(Error::INTERNAL);
    }
    AsicenEnclosureFamily* family_view = asicen_family.get();
    const auto prepared = family_view->prepare_initial_receiver(stop_requested);
    if (!prepared) return Result<std::unique_ptr<Owner>>::failure(prepared.error());

    std::unique_ptr<Family> family(std::move(asicen_family));
    std::unique_ptr<PlatformTunerNonceSource> nonce(new (std::nothrow)
        PlatformTunerNonceSource());
    if (!nonce) return Result<std::unique_ptr<Owner>>::failure(Error::INTERNAL);
    const auto epoch = nonce->generate();
    if (!epoch) return Result<std::unique_ptr<Owner>>::failure(epoch.error());

    auto assembled = Owner::assemble(std::move(family), std::move(nonce));
    if (!assembled) return Result<std::unique_ptr<Owner>>::failure(assembled.error());
    std::unique_ptr<Owner> owner = std::move(assembled.value());
    const auto adopted = owner->adopt_namespace(publication, std::move(lease));
    if (!adopted) return Result<std::unique_ptr<Owner>>::failure(adopted.error());

    px4::userland::ipc::posix::ControlContext control_context;
    if (!build_context_from_claims(
            family_view->hardware(), owner->tuner_service(),
            publication.instance.c_str(), epoch.value(), control_context)) {
        return Result<std::unique_ptr<Owner>>::failure(Error::INTERNAL);
    }
    const auto published = owner->publish(control_context);
    if (!published) return Result<std::unique_ptr<Owner>>::failure(published.error());
    std::fprintf(stderr,
                 "asicend ready backend=asicen-libusb-live-card-stream model=%s "
#if defined(_WIN32)
                 "receivers=%u endpoint=%ls\n",
#else
                 "receivers=%u endpoint=%s\n",
#endif
                 device_profile->model_key,
                 static_cast<unsigned>(family_view->hardware().receiver_count()),
                 owner->endpoint_path());
    return Result<std::unique_ptr<Owner>>::success(std::move(owner));
}

OwnedLiveCardStreamResult run_live_card_stream_server(
    px4::userland::ipc::posix::EnclosureContext& context,
    const volatile std::sig_atomic_t* stop_requested,
    bool (*stop_callback)() noexcept,
    std::chrono::milliseconds close_budget) noexcept
{
    Error loop_error = Error::OK;
    while ((stop_requested == nullptr || *stop_requested == 0) &&
           (stop_callback == nullptr || !stop_callback())) {
        const auto polled = context.poll_once(px4::userland::Timeout{100U});
        if (!polled) {
            std::fprintf(stderr, "asicend poll: %s\n", px4::userland::error_string(polled.error()));
            loop_error = polled.error();
            break;
        }
    }
    const auto closed = context.close(close_budget);
    if (!closed.quiesced) return OwnedLiveCardStreamResult{70, closed.deadline_expired};
    if (loop_error == Error::OK && closed.error != Error::OK) loop_error = closed.error;
    return OwnedLiveCardStreamResult{exit_status(loop_error), false};
}

}  // namespace asicen
