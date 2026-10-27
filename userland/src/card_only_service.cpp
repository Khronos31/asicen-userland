// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/card_only_service.h"
#include "dtv/userland/detail/diagnostic_log.h"

#include "asicen/libusb_hardware_backend.h"
#include "asicen/hardware_stream_session.h"
#include "asicen/enclosure.h"
#include "asicen/product_profile.h"
#include "asicen/usb_location_record.h"
#include "dtv/userland/host_listener_context/enclosure_context.h"
#include "dtv/userland/host_runtime/control_socket.h"
#include "dtv/userland/host_runtime/family.h"
#include "dtv/userland/records.h"
#if defined(_WIN32)
#include "px4/windows_tuner_nonce.h"
#else
#include "px4/posix_tuner_nonce.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

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
    case Error::INTERNAL:
        return 70;
    case Error::RESOURCE_LIMIT:
        return 70;
    }
    return 70;
}

void print_listener_diagnostic(
    const dtv::userland::host_listener_context::ListenerDiagnostic& diagnostic) noexcept
{
    using Reason = dtv::userland::host_listener_context::ListenerDiagnosticReason;
    if (diagnostic.reason == Reason::endpoint_path) {
        if (diagnostic.endpoint.reason ==
            dtv::userland::host_runtime::EndpointDiagnosticReason::path_too_long) {
            std::fprintf(stderr,
                "asicend endpoint: %.*s path is too long (%zu UTF-8 bytes; %zu bytes including NUL required, this build's native sun_path capacity is %zu bytes including NUL); shorten --runtime-dir or --instance consistently for daemon and clients\n",
                static_cast<int>(diagnostic.endpoint_name.size()),
                diagnostic.endpoint_name.data(), diagnostic.endpoint.path_bytes,
                diagnostic.endpoint.required_bytes,
                diagnostic.endpoint.native_capacity);
        } else {
            std::fprintf(stderr,
                "asicend endpoint: invalid runtime/instance/endpoint path configuration for %.*s; check --runtime-dir and --instance\n",
                static_cast<int>(diagnostic.endpoint_name.size()),
                diagnostic.endpoint_name.data());
        }
        return;
    }
    if (diagnostic.reason == Reason::runtime_directory_security) {
        std::fprintf(stderr,
            "asicend endpoint: runtime namespace security check failed; the configured runtime and its product/version/instance directories must satisfy the current-user ownership and private access checks\n");
        return;
    }
    const char* directory = "runtime directory";
    switch (diagnostic.directory_layer) {
    case dtv::userland::host_listener_context::RuntimeDirectoryLayer::runtime_root:
        directory = "runtime root";
        break;
    case dtv::userland::host_listener_context::RuntimeDirectoryLayer::product:
        directory = "product directory";
        break;
    case dtv::userland::host_listener_context::RuntimeDirectoryLayer::version:
        directory = "version directory";
        break;
    case dtv::userland::host_listener_context::RuntimeDirectoryLayer::instance:
        directory = "instance directory";
        break;
    }
    if (diagnostic.reason == Reason::runtime_directory_missing)
        std::fprintf(stderr, "asicend endpoint: %s does not exist\n", directory);
    else if (diagnostic.reason == Reason::runtime_directory_lookup)
        std::fprintf(stderr, "asicend endpoint: %s lookup failed; check path traversal access\n",
                     directory);
    else if (diagnostic.reason == Reason::runtime_directory_type)
        std::fprintf(stderr, "asicend endpoint: %s type check failed; it must be a real directory, not a symlink\n",
                     directory);
    else if (diagnostic.reason == Reason::runtime_directory_owner)
        std::fprintf(stderr, "asicend endpoint: %s owner check failed; it must be owned by the current user\n",
                     directory);
    else if (diagnostic.reason == Reason::runtime_directory_mode)
        std::fprintf(stderr, "asicend endpoint: %s mode check failed; private runtime directories must not grant group or other access\n",
                     directory);
    else if (diagnostic.reason == Reason::runtime_directory_encoding)
        std::fprintf(stderr, "asicend endpoint: --runtime-dir must be valid UTF-8 on Windows\n");
}

int core_exit_status(dtv::userland::Error error) noexcept
{
    switch (error) {
    case dtv::userland::Error::OK:
        return 0;
    case dtv::userland::Error::INVALID_ARGUMENT:
        return 2;
    case dtv::userland::Error::NOT_FOUND:
    case dtv::userland::Error::NOT_READY:
    case dtv::userland::Error::NO_CARD:
    case dtv::userland::Error::UNSUPPORTED:
        return 3;
    case dtv::userland::Error::BUSY:
        return 4;
    case dtv::userland::Error::TIMEOUT:
        return 5;
    case dtv::userland::Error::VERSION_MISMATCH:
    case dtv::userland::Error::PROTOCOL_ERROR:
        return 6;
    case dtv::userland::Error::USB_IO:
    case dtv::userland::Error::DISCONNECTED:
        return 7;
    case dtv::userland::Error::SLOW_CONSUMER:
        return 8;
    case dtv::userland::Error::CARD_REMOVED:
    case dtv::userland::Error::BUFFER_TOO_SMALL:
        return 9;
    case dtv::userland::Error::FIRMWARE_REJECTED:
        return 10;
    case dtv::userland::Error::RESOURCE_LIMIT:
    case dtv::userland::Error::INTERNAL:
        return 70;
    }
    return 70;
}
#if defined(_WIN32)
using PlatformTunerNonceSource = px4::userland::ipc::windows::WindowsTunerNonceSource;
#else
using PlatformTunerNonceSource = px4::userland::ipc::posix::PosixTunerNonceSource;
#endif

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
    Result<void> set_independent_lnb_power(bool enabled) noexcept override
    {
        return hardware_.set_independent_lnb_power(enabled);
    }
    dtv::userland::wire::Optional<bool> independent_lnb_power_state() noexcept override
    {
        return hardware_.independent_lnb_power_state();
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

template <std::size_t N>
bool set_text(dtv::userland::wire::WString<N>& output,
              std::string_view value) noexcept
{
    if (value.size() > N) return false;
    output.length = static_cast<std::uint16_t>(value.size());
    std::memcpy(output.bytes.data(), value.data(), value.size());
    return true;
}

class AsicenEnclosureFamily final : public dtv::userland::host_runtime::EnclosureFamily {
public:
    AsicenEnclosureFamily(LibusbW3u3Hardware& hardware,
                          W3u3CardMailboxHardware& mailbox,
                          const volatile std::sig_atomic_t* stop_requested,
                          bool card_only) noexcept
        : hardware_(hardware), card_backend_(mailbox, hardware, stop_requested),
          card_session_(mailbox, mailbox), card_protocol_(card_session_, hardware, stop_requested),
          card_only_tuner_(hardware.receiver_count()), frontend_(hardware)
    {
        if (!card_only)
            stream_.reset(new (std::nothrow) HardwareStreamService(frontend_, hardware_));
    }

    bool ready(bool card_only) const noexcept { return card_only || stream_ != nullptr; }
    px4::userland::TunerServiceBackend& tuner_backend() noexcept override
    {
        return stream_ != nullptr ? static_cast<px4::userland::TunerServiceBackend&>(*stream_)
                                  : static_cast<px4::userland::TunerServiceBackend&>(card_only_tuner_);
    }
    px4::userland::CardServiceBackend& card_backend() noexcept override
    {
        return card_backend_;
    }
    px4::userland::CardProtocolSession& card_protocol() noexcept override
    {
        return card_protocol_;
    }
    px4::userland::TunerServiceTime& time() noexcept override { return time_; }
    px4::userland::TunerStreamControl* stream_control() noexcept override
    {
        return stream_.get();
    }

private:
    LibusbW3u3Hardware& hardware_;
    W3u3CardServiceBackend card_backend_;
    px4::userland::CardSession card_session_;
    W3u3CardProtocolSession card_protocol_;
    CardOnlyTunerBackend card_only_tuner_;
    DeferredHardwareShutdown frontend_;
    std::unique_ptr<HardwareStreamService> stream_;
    CardOnlyTime time_;
};

bool make_list(const DeviceProfile& profile, std::uint8_t receiver_count,
               UsbLocation primary_location, UsbLocation sibling_location,
               const std::vector<std::uint8_t>& primary_path,
               const std::vector<std::uint8_t>& sibling_path,
               const char* version, bool lnb_power,
               dtv::userland::wire::ListResponse& list) noexcept
{
    using namespace dtv::userland::wire;
    if (!set_text(list.producer.product, "asicen-userland") ||
        !set_text(list.producer.version, version)) return false;
    list.topology.state = TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = profile.expected_runtime_functions;
    list.enclosure.present = true;
    list.enclosure.value.id = 0U;
    list.enclosure.value.model.present = true;
    if (!set_text(list.enclosure.value.model.value.name, profile.model)) return false;
    list.enclosure.value.identity.serial_state = IdentitySerialState::unknown;
    list.enclosure.value.identity.uniqueness = Uniqueness::not_applicable;
    list.enclosure.value.limits.max_concurrent_streams.present = true;
    list.enclosure.value.limits.max_concurrent_streams.value =
        profile.enclosure_receiver_count == 4U ? receiver_count : 1U;
    list.functions.count = profile.expected_runtime_functions;
    for (std::size_t index = 0U; index < list.functions.count; ++index) {
        UsbFunction& function = list.functions.items[index];
        function.id = static_cast<std::uint16_t>(index + 1U);
        function.vid = profile.vid;
        function.pid = profile.pid;
        function.mode = Mode::runtime;
        function.model.present = true;
        if (!set_text(function.model.value.name, profile.model)) return false;
        if (!set_usb_location_record(function.location,
                index == 0U ? primary_location : sibling_location,
                index == 0U ? primary_path : sibling_path)) return false;
        function.present = true;
    }
    list.receivers.present = true;
    if (receiver_count > profile.enclosure_receiver_count) return false;
    list.receivers.value.count = receiver_count;
    for (std::size_t index = 0U; index < profile.enclosure_receiver_count; ++index) {
        const auto address = receiver_address(profile, index);
        if (!address) return false;
        ReceiverDescriptor& receiver = list.receivers.value.items[index];
        receiver.id = static_cast<std::uint16_t>(index);
        receiver.systems.mask = profile.combined_isdb_ts ? 0x03U
            : (address.value().system == BroadcastSystem::IsdbT ? 0x01U : 0x02U);
        receiver.binding.present = true;
        receiver.binding.value.function_id =
            static_cast<std::uint16_t>(address.value().function_index + 1U);
        receiver.binding.value.local_id = address.value().local_lane;
        receiver.capabilities.ts_scope = TsScope::multiplex;
        if (lnb_power && (receiver.systems.mask & 0x02U) != 0U) {
            receiver.capabilities.controls |= kControlBitLnb;
            receiver.capabilities.lnb.values.count = 2U;
            receiver.capabilities.lnb.values.items[0U] = 0U;
            receiver.capabilities.lnb.values.items[1U] = 15U;
        }
    }
    list.readers.present = true;
    list.readers.value.count = 1U;
    list.readers.value.items[0U].id = 0U;
    list.readers.value.items[0U].binding.present = true;
    list.readers.value.items[0U].binding.value.function_id = 1U;
    list.readers.value.items[0U].binding.value.local_id = 0U;
    return true;
}

int run_core_server(LibusbW3u3Hardware& hardware, W3u3CardMailboxHardware& mailbox,
                    const char* runtime_directory,
                    const char* instance, bool group, bool card_only,
                    UsbLocation primary_location, UsbLocation sibling_location,
                    const std::vector<std::uint8_t>& primary_path,
                    const std::vector<std::uint8_t>& sibling_path,
                    const volatile std::sig_atomic_t* stop_requested,
                    bool (*stop_callback)() noexcept) noexcept
{
    using namespace dtv::userland;
    using dtv::userland::host_listener_context::CloseState;
    using dtv::userland::host_listener_context::ContextConfig;
    using dtv::userland::host_listener_context::EnclosureContext;
    using dtv::userland::wire::ListResponse;

    const auto* profile = hardware.device_profile();
    if (profile == nullptr || !profile_runtime_supported(*profile)) return 3;
    const std::uint8_t initial_receiver = profile->combined_isdb_ts ? 0U : 1U;
    if (card_only) {
        const auto opened = hardware.open_receiver(initial_receiver);
        if (!opened) {
            dtv::userland::detail::diagnostic_log(
                "asicend_setup_failure stage=open_receiver receiver=%u error=%u",
                static_cast<unsigned>(initial_receiver),
                static_cast<unsigned>(opened.error()));
            return exit_status(opened.error());
        }
    } else {
        const auto prepared = hardware.prepare_runtime_functions();
        if (!prepared) {
            dtv::userland::detail::diagnostic_log(
                "asicend_setup_failure stage=prepare_runtime_functions error=%u",
                static_cast<unsigned>(prepared.error()));
            return exit_status(prepared.error());
        }
    }

    std::unique_ptr<AsicenEnclosureFamily> family(new (std::nothrow) AsicenEnclosureFamily(
        hardware, mailbox, stop_requested, card_only));
    if (!family || !family->ready(card_only)) {
        dtv::userland::detail::diagnostic_log(
            "asicend_first_fatal stage=family_ready reason=not_ready");
        return 70;
    }

    std::unique_ptr<PlatformTunerNonceSource> nonce(new (std::nothrow) PlatformTunerNonceSource());
    if (!nonce) {
        dtv::userland::detail::diagnostic_log(
            "asicend_first_fatal stage=nonce_allocation reason=resource_limit");
        return 70;
    }
    const auto generated_epoch = nonce->generate();
    if (!generated_epoch) return exit_status(generated_epoch.error());

    ContextConfig config;
    if (runtime_directory != nullptr && runtime_directory[0] != '\0') {
        config.runtime_directory = runtime_directory;
    } else {
#if !defined(_WIN32)
        const char* runtime_root = std::getenv("XDG_RUNTIME_DIR");
        if (runtime_root == nullptr || runtime_root[0] == '\0') return 2;
        config.runtime_directory = runtime_root;
#else
        return 2;
#endif
    }
    config.product = "asicen-userland";
    config.instance = instance;
    config.access = group ? host_runtime::EndpointAccess::shared_group
                          : host_runtime::EndpointAccess::private_user;
    std::copy(generated_epoch.value().begin(), generated_epoch.value().end(),
              config.server.epoch.begin());
    config.server.features = wire::kFeatureStateEvent | wire::kFeatureCardService;
    ListResponse list;
    if (!make_list(*profile, hardware.receiver_count(), primary_location,
                   sibling_location, primary_path, sibling_path,
                   ASICEN_PROJECT_VERSION,
                   !card_only && hardware.supports_lnb_control(), list)) {
        dtv::userland::detail::diagnostic_log(
            "asicend_first_fatal stage=make_list reason=invalid_profile_data");
        return 70;
    }
    list.epoch = config.server.epoch;
    config.server.producer = list.producer;
    config.canonical_list = list;
    config.context.epoch = config.server.epoch;
    config.context.instance.present = true;
    if (!set_text(config.context.instance.value, instance)) return 2;
    config.context.state = wire::ContextState::ready;
    std::uint64_t engine_epoch = 0U;
    for (std::size_t index = 0U; index < sizeof(engine_epoch); ++index)
        engine_epoch = (engine_epoch << 8U) | generated_epoch.value()[index];
    config.stream_engine_epoch = engine_epoch == 0U ? 1U : engine_epoch;
    config.stream_arm_budget_ms = 5000U;

    std::unique_ptr<host_runtime::EnclosureFamily> owned_family(family.release());
    std::unique_ptr<host_runtime::TunerNonceSource> owned_nonce(nonce.release());
    auto assembled = EnclosureContext::assemble(std::move(owned_family),
                                                 std::move(owned_nonce), config);
    if (!assembled) {
        dtv::userland::detail::diagnostic_log(
            "asicend_first_fatal stage=assemble error=%u",
            static_cast<unsigned>(assembled.error()));
        return core_exit_status(assembled.error());
    }
    std::unique_ptr<EnclosureContext> context = std::move(assembled.value());
    dtv::userland::host_listener_context::ListenerDiagnostic listener_diagnostic{};
    const auto reserved = context->reserve_namespace(&listener_diagnostic);
    if (!reserved) {
        if (listener_diagnostic.reason !=
            dtv::userland::host_listener_context::ListenerDiagnosticReason::none)
            print_listener_diagnostic(listener_diagnostic);
        dtv::userland::detail::diagnostic_log(
            "asicend_first_fatal stage=reserve_namespace error=%u",
            static_cast<unsigned>(reserved.error()));
        return core_exit_status(reserved.error());
    }
    const auto published = context->publish();
    if (!published) {
        dtv::userland::detail::diagnostic_log(
            "asicend_first_fatal stage=publish error=%u",
            static_cast<unsigned>(published.error()));
        return core_exit_status(published.error());
    }

    const host_runtime::EndpointConfig endpoint{
        config.runtime_directory, config.product, config.instance, config.access};
    const auto endpoint_path = host_runtime::control_endpoint_path(endpoint);
    std::fprintf(stderr, "asicend ready backend=dtv-core model=%s receivers=%u endpoint=%s\n",
                 profile->model_key, static_cast<unsigned>(hardware.receiver_count()),
                 endpoint_path ? endpoint_path.value().c_str() : "unknown");
    dtv::userland::Error loop_error = dtv::userland::Error::OK;
    while ((stop_requested == nullptr || *stop_requested == 0) &&
           (stop_callback == nullptr || !stop_callback())) {
        const auto polled = context->poll_once(Timeout{100U});
        if (!polled) {
            loop_error = polled.error();
            dtv::userland::detail::diagnostic_log(
                "asicend_first_fatal stage=poll_once error=%u",
                static_cast<unsigned>(loop_error));
            break;
        }
    }

    for (;;) {
        const auto closed = context->close(Timeout{5000U});
        if (!closed) {
            if (loop_error == dtv::userland::Error::OK) loop_error = closed.error();
            break;
        }
        if (closed.value() == CloseState::quiesced) break;
    }
    std::fprintf(stderr, "asicend daemon exit error=%s code=%u\n",
                 dtv::userland::error_string(loop_error),
                 static_cast<unsigned>(loop_error));
    return core_exit_status(loop_error);
}

}  // namespace

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
    if (result) {
        atr_.bytes = session_.atr().bytes;
        atr_.length = session_.atr().length;
    }
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

const px4::userland::CardAtrBytes& W3u3CardProtocolSession::atr() const noexcept
{
    return atr_;
}

void W3u3CardProtocolSession::invalidate() noexcept
{
    session_.invalidate();
    atr_ = {};
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
    if (device_profile == nullptr || !profile_runtime_supported(*device_profile)) return 3;
    W3u3CardMailboxHardware mailbox(static_cast<FrontendTransport&>(hardware),
                                    device_profile->model_id);
    const int result = run_core_server(hardware, mailbox, runtime_directory, instance, group,
                                       true, hardware.primary_location_,
                                       hardware.sibling_location_, hardware.primary_path_,
                                       hardware.sibling_path_, stop_requested, stop_callback);
    hardware.diagnostic_stop_flag_ = nullptr;
    return result;
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
    if (device_profile == nullptr || !profile_runtime_supported(*device_profile)) return 3;
    W3u3CardMailboxHardware mailbox(static_cast<FrontendTransport&>(hardware),
                                    device_profile->model_id);
    const int result = run_core_server(hardware, mailbox, runtime_directory, instance, group,
                                       false, hardware.primary_location_,
                                       hardware.sibling_location_, hardware.primary_path_,
                                       hardware.sibling_path_, stop_requested, stop_callback);
    hardware.diagnostic_stop_flag_ = nullptr;
    return result;
}

}  // namespace asicen
