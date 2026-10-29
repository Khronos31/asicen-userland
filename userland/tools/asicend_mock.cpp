// SPDX-License-Identifier: GPL-2.0-or-later
// asicend-mock: isolated mock tuner/card service for offline testing and
// research.  It has no hardware backend and is not part of the distributed
// product; the real-hardware daemon is asicend.
#include "asicen/product_profile.h"
#include "asicen/device_profile.h"
#include "asicen/enclosure.h"
#include "asicen/px4_mock_backend.h"
#include "asicend_args.h"
#include "dtv/userland/host_listener_context/enclosure_context.h"
#include "dtv/userland/host_runtime/control_socket.h"
#include "dtv/userland/records.h"
#include "px4/posix_tuner_nonce.h"
#include "px4d_signals.h"

#include <signal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

namespace w = dtv::userland::wire;
using namespace dtv::userland;
using namespace dtv::userland::host_listener_context;
using namespace dtv::userland::host_runtime;
using namespace px4::userland;

class MockTime final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t ms) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
};

class Family final : public EnclosureFamily {
public:
    Family(const asicen::DeviceProfile& profile, bool allow_lnb_power) noexcept
        : backend_(profile)
    {
        backend_.set_allow_lnb_power(allow_lnb_power);
    }
    TunerServiceBackend& tuner_backend() noexcept override { return backend_; }
    CardServiceBackend& card_backend() noexcept override { return card_backend_; }
    CardProtocolSession& card_protocol() noexcept override { return card_session_; }
    TunerServiceTime& time() noexcept override { return time_; }
    TunerStreamControl* stream_control() noexcept override { return &stream_; }

private:
    asicen::MockTunerBackend backend_;
    asicen::UnsupportedCardBackend card_backend_;
    asicen::UnsupportedCardSession card_session_;
    px4::userland::ipc::posix::PosixTunerNonceSource nonce_;
    MockTime time_;
    asicen::MockTunerStream stream_;
};

template <std::size_t N>
bool set_text(w::WString<N>& output, const char* value) noexcept
{
    const std::size_t length = std::strlen(value);
    if (length > N) return false;
    output.length = static_cast<std::uint16_t>(length);
    std::memcpy(output.bytes.data(), value, length);
    return true;
}

bool make_list(const asicen::DeviceProfile& profile, const char* version,
               const w::Uuid& epoch, w::ListResponse& list) noexcept
{
    if (!set_text(list.producer.product, "asicen-userland") ||
        !set_text(list.producer.version, version)) return false;
    list.epoch = epoch;
    list.topology.state = w::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = profile.expected_runtime_functions;
    list.functions.count = profile.expected_runtime_functions;
    for (std::size_t index = 0U; index < list.functions.count; ++index) {
        auto& function = list.functions.items[index];
        function.id = static_cast<std::uint16_t>(index + 1U);
        function.vid = profile.vid;
        function.pid = profile.pid;
        function.mode = w::Mode::runtime;
        function.serial_state = w::SerialState::absent;
        function.identification_reason = w::IdentificationReason::descriptor_unreadable;
        function.present = true;
    }
    list.enclosure.present = true;
    list.enclosure.value.id = 0U;
    list.enclosure.value.model.present = true;
    if (!set_text(list.enclosure.value.model.value.name, profile.model)) return false;
    list.enclosure.value.identity.serial_state = w::IdentitySerialState::absent;
    list.enclosure.value.identity.uniqueness = w::Uniqueness::not_applicable;
    list.receivers.present = true;
    list.receivers.value.count = profile.enclosure_receiver_count;
    for (std::size_t index = 0U; index < profile.enclosure_receiver_count; ++index) {
        const auto address = asicen::receiver_address(profile, index);
        if (!address) return false;
        auto& receiver = list.receivers.value.items[index];
        receiver.id = static_cast<std::uint16_t>(index);
        receiver.systems.mask = profile.combined_isdb_ts ? 0x03U
            : (address.value().system == asicen::BroadcastSystem::IsdbT ? 0x01U : 0x02U);
        receiver.binding.present = true;
        receiver.binding.value.function_id =
            static_cast<std::uint16_t>(address.value().function_index + 1U);
        receiver.binding.value.local_id = address.value().local_lane;
        receiver.capabilities.ts_scope = w::TsScope::multiplex;
        const bool has_lnb_control = profile.model_id == asicen::ModelId::W3u2 ||
            profile.model_id == asicen::ModelId::W3u3 ||
            profile.model_id == asicen::ModelId::W3u3V2;
        if (has_lnb_control && address.value().system == asicen::BroadcastSystem::IsdbS) {
            receiver.capabilities.controls |= w::kControlBitLnb;
            receiver.capabilities.lnb.values.count = 2U;
            receiver.capabilities.lnb.values.items[0U] = 0U;
            receiver.capabilities.lnb.values.items[1U] = 15U;
        }
    }
    list.readers.present = true;
    list.readers.value.count = 1U;
    list.readers.value.items[0U].id = 0U;
    const auto validation = w::validate_list_response(list);
    if (validation != Error::OK)
        std::fprintf(stderr, "asicend-mock: invalid canonical list (%u)\n",
                     static_cast<unsigned>(validation));
    return validation == Error::OK;
}

int exit_status(Error error) noexcept
{
    switch (error) {
    case Error::OK: return 0;
    case Error::INVALID_ARGUMENT: return 2;
    case Error::NOT_FOUND:
    case Error::NOT_READY:
    case Error::NO_CARD:
    case Error::UNSUPPORTED: return 3;
    case Error::BUSY: return 4;
    case Error::TIMEOUT: return 5;
    case Error::VERSION_MISMATCH:
    case Error::PROTOCOL_ERROR: return 6;
    case Error::USB_IO:
    case Error::DISCONNECTED: return 7;
    case Error::SLOW_CONSUMER: return 8;
    case Error::CARD_REMOVED:
    case Error::BUFFER_TOO_SMALL: return 9;
    case Error::FIRMWARE_REJECTED: return 10;
    case Error::RESOURCE_LIMIT:
    case Error::INTERNAL: return 70;
    }
    return 70;
}

}  // namespace

int main(int argc, char** argv)
{
    const auto arguments = asicen::cli::parse_daemon_arguments(
        argc, const_cast<const char* const*>(argv), true);
    if (!arguments.valid) {
        std::fprintf(stderr, "argument error: %.*s\n",
                     static_cast<int>(arguments.error.size()), arguments.error.data());
        return 2;
    }
    if (arguments.help) {
        std::puts("usage: asicend-mock [--model MODEL] [--runtime-dir PATH] [--instance TOKEN]\n"
                  "                    [--group] [--allow-lnb-power]");
        return 0;
    }
    const asicen::DeviceProfile* profile = arguments.expected_model != nullptr
        ? arguments.expected_model : asicen::find_profile(asicen::ModelId::W3u3);
    if (profile == nullptr) return 2;

    auto family = std::unique_ptr<EnclosureFamily>(new (std::nothrow)
        Family(*profile, arguments.allow_lnb_power));
    auto nonce = std::unique_ptr<TunerNonceSource>(new (std::nothrow)
        px4::userland::ipc::posix::PosixTunerNonceSource());
    if (!family || !nonce) return 70;
    ContextConfig config;
    if (!arguments.runtime_directory.empty()) {
        config.runtime_directory = arguments.runtime_directory;
    } else {
        const char* runtime_root = std::getenv("XDG_RUNTIME_DIR");
        if (runtime_root == nullptr || runtime_root[0] == '\0') return 2;
        config.runtime_directory = runtime_root;
    }
    config.product = "asicen-userland";
    config.instance = arguments.instance;
    config.access = arguments.group ? EndpointAccess::shared_group : EndpointAccess::private_user;
    const auto generated_epoch = nonce->generate();
    if (!generated_epoch) return exit_status(generated_epoch.error());
    std::copy(generated_epoch.value().begin(), generated_epoch.value().end(),
              config.server.epoch.begin());
    config.server.features = w::kFeatureCardService;
    if (!make_list(*profile, ASICEN_PROJECT_VERSION, config.server.epoch,
                   config.canonical_list)) return 70;
    config.server.producer = config.canonical_list.producer;
    config.context.epoch = config.server.epoch;
    config.context.instance.present = true;
    if (!set_text(config.context.instance.value, arguments.instance.c_str())) return 2;
    config.context.state = w::ContextState::ready;
    for (std::size_t index = 0U; index < sizeof(config.stream_engine_epoch); ++index)
        config.stream_engine_epoch = (config.stream_engine_epoch << 8U) |
                                     generated_epoch.value()[index];
    if (config.stream_engine_epoch == 0U) config.stream_engine_epoch = 1U;
    config.stream_arm_budget_ms = 5000U;

    auto assembled = EnclosureContext::assemble(std::move(family), std::move(nonce), config);
    if (!assembled) return exit_status(assembled.error());
    auto context = std::move(assembled.value());
    const auto reserved = context->reserve_namespace();
    if (!reserved) return exit_status(reserved.error());
    const auto published = context->publish();
    if (!published) return exit_status(published.error());
    std::fprintf(stderr, "asicend-mock ready backend=mock-only model=%s receivers=%u\n",
                 profile->model_key, static_cast<unsigned>(profile->enclosure_receiver_count));
    if (!px4::userland::px4d::install_signal_handlers()) return 70;
    Error loop_error = Error::OK;
    while (!px4::userland::px4d::stop_requested()) {
        const auto polled = context->poll_once(Timeout{100U});
        if (!polled) {
            loop_error = static_cast<Error>(polled.error());
            std::fprintf(stderr, "asicend-mock error stage=poll_once class=%s code=%u\n",
                         px4::userland::error_string(loop_error),
                         static_cast<unsigned>(loop_error));
            break;
        }
    }
    for (;;) {
        const auto closed = context->close(Timeout{5000U});
        if (!closed) {
            const Error close_error = static_cast<Error>(closed.error());
            std::fprintf(stderr, "asicend-mock error stage=close class=%s code=%u\n",
                         px4::userland::error_string(close_error),
                         static_cast<unsigned>(close_error));
            if (loop_error == Error::OK) loop_error = static_cast<Error>(closed.error());
            break;
        }
        if (closed.value() == CloseState::quiesced) break;
    }
    px4::userland::px4d::notify_cleanup_complete();
    return exit_status(loop_error);
}
