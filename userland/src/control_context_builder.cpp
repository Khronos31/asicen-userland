// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/control_context_builder.h"
#include "asicen/product_profile.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace asicen {
namespace {
namespace wire = dtv::userland::wire;

template <std::size_t N>
bool copy_text(wire::WString<N>& out, const char* value) noexcept
{
    if (value == nullptr) return false;
    const std::size_t size = std::strlen(value);
    if (size > N) return false;
    out.length = static_cast<std::uint16_t>(size);
    for (std::size_t i = 0U; i < size; ++i) out.bytes[i] = value[i];
    return true;
}
}  // namespace

bool build_asicen_control_context(
    const DeviceProfile& profile, const ClaimedUsbFunctionObservation* observations,
    std::uint8_t function_count, std::uint8_t receiver_count,
    const px4::userland::TunerService& tuner, const char* instance,
    const wire::Epoch& epoch,
    px4::userland::ipc::posix::ControlContext& out, bool tuner_enabled) noexcept
{
    if (!profile_runtime_supported(profile) || observations == nullptr ||
        profile.expected_runtime_functions == 0U ||
        profile.expected_runtime_functions > wire::kMaxFunctions ||
        function_count != profile.expected_runtime_functions ||
        receiver_count != (profile.combined_isdb_ts ? 1U : 2U) ||
        receiver_count > wire::kMaxPublicReceivers ||
        (tuner_enabled && tuner.receiver_count() < receiver_count)) return false;

    out = px4::userland::ipc::posix::ControlContext{};
    out.epoch = epoch;
    if (instance != nullptr && instance[0] != '\0') {
        out.instance.present = copy_text(out.instance.value, instance);
        if (!out.instance.present) return false;
    }
    wire::ListResponse& list = out.list;
    list.epoch = epoch;
    if (!copy_text(list.producer.product, "asicen-userland") ||
        !copy_text(list.producer.version, ASICEN_PRODUCT_VERSION)) return false;
    list.topology.state = wire::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = profile.expected_runtime_functions;
    list.enclosure.present = true;
    wire::Enclosure& enclosure = list.enclosure.value;
    enclosure.id = 0U;
    enclosure.model.present = true;
    if (!copy_text(enclosure.model.value.id, profile.model_key) ||
        !copy_text(enclosure.model.value.name, profile.model)) return false;
    enclosure.identity.serial_state = wire::IdentitySerialState::absent;
    enclosure.identity.uniqueness = wire::Uniqueness::not_applicable;
    if (tuner_enabled) {
        enclosure.limits.max_concurrent_streams.present = true;
        enclosure.limits.max_concurrent_streams.value = 1U;
    }

    list.functions.count = function_count;
    for (std::uint16_t i = 0U; i < list.functions.count; ++i) {
        wire::UsbFunction& function = list.functions.items[i];
        const ClaimedUsbFunctionObservation& observed = observations[i];
        if (observed.vendor_id != profile.vid || observed.product_id != profile.pid)
            return false;
        function.id = i;
        function.vid = observed.vendor_id;
        function.pid = observed.product_id;
        function.mode = wire::Mode::runtime;
        function.serial_state = wire::SerialState::absent;
        if (observed.has_bus) {
            function.location.bus.present = true;
            function.location.bus.value = observed.bus;
        }
        if (observed.has_address) {
            function.location.address.present = true;
            function.location.address.value = observed.address;
        }
        if (observed.has_bus && !observed.port_path.empty()) {
            char path[64]{};
            int length = std::snprintf(path, sizeof(path), "%u", observed.bus);
            if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(path)) return false;
            for (std::size_t port = 0U; port < observed.port_path.size(); ++port) {
                const int written = std::snprintf(path + length, sizeof(path) -
                    static_cast<std::size_t>(length), port == 0U ? "-%u" : ".%u",
                    observed.port_path[port]);
                if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(path) -
                    static_cast<std::size_t>(length)) return false;
                length += written;
            }
            if (static_cast<std::size_t>(length) > function.location.usb_path.value.bytes.size())
                return false;
            function.location.usb_path.present = true;
            function.location.usb_path.value.length = static_cast<std::uint16_t>(length);
            for (int c = 0; c < length; ++c)
                function.location.usb_path.value.bytes[static_cast<std::size_t>(c)] = path[c];
        }
        function.model.present = true;
        function.model.value = enclosure.model.value;
        function.present = true;
    }

    list.receivers.present = true;
    list.receivers.value.count = tuner_enabled ? receiver_count : 0U;
    out.execution.receiver_count = tuner_enabled ? receiver_count : 0U;
    for (std::uint8_t i = 0U; tuner_enabled && i < receiver_count; ++i) {
        wire::ReceiverDescriptor& receiver = list.receivers.value.items[i];
        receiver.id = i;
        const bool terrestrial = tuner.receiver_supports(i, px4::userland::ipc::System::ISDB_T);
        const bool satellite = tuner.receiver_supports(i, px4::userland::ipc::System::ISDB_S);
        receiver.systems.mask = static_cast<std::uint8_t>((terrestrial ? 1U : 0U) |
                                                          (satellite ? 2U : 0U));
        if (receiver.systems.mask == 0U) return false;
        receiver.binding.present = true;
        receiver.binding.value.function_id = 0U;
        receiver.binding.value.local_id = i;
        receiver.capabilities.ts_scope = wire::TsScope::multiplex;
        if (satellite) {
            const auto strict = tuner.strict_profile(i, px4::userland::ipc::System::ISDB_S);
            if ((strict.lnb_voltage_mask & 1U) != 0U)
                receiver.capabilities.lnb.values.items[receiver.capabilities.lnb.values.count++] = 0U;
            if ((strict.lnb_voltage_mask & 2U) != 0U)
                receiver.capabilities.lnb.values.items[receiver.capabilities.lnb.values.count++] = 15U;
            if (receiver.capabilities.lnb.values.count != 0U)
                receiver.capabilities.controls |= wire::kControlBitLnb;
        }
        out.receiver_index[i] = i;
        out.execution.receiver_lane[i] =
            profile.expected_runtime_functions > 1U &&
                    i >= asicen::profile::kReceiversPerUsbFunction
                ? px4::userland::ipc::posix::ControlWorkerLane::tuner_dev2
                : px4::userland::ipc::posix::ControlWorkerLane::tuner_dev1;
    }
    list.readers.present = true;
    list.readers.value.count = 1U;
    list.readers.value.items[0].id = 0U;
    list.readers.value.items[0].binding.present = true;
    list.readers.value.items[0].binding.value = wire::Binding{0U, 0U};
    return wire::validate_list_response(list) == dtv::userland::Error::OK;
}

bool build_asicen_mock_control_context(
    const DeviceProfile& profile, const px4::userland::TunerService& tuner,
    const char* instance, const wire::Epoch& epoch,
    px4::userland::ipc::posix::ControlContext& out) noexcept
{
    if (profile.expected_runtime_functions == 0U ||
        profile.expected_runtime_functions > 2U) return false;
    std::array<ClaimedUsbFunctionObservation, 2U> observations{};
    for (std::uint8_t i = 0U; i < profile.expected_runtime_functions; ++i) {
        observations[i].vendor_id = profile.vid;
        observations[i].product_id = profile.pid;
    }
    const std::uint8_t receivers = profile.combined_isdb_ts ? 1U : 2U;
    return build_asicen_control_context(profile, observations.data(),
        profile.expected_runtime_functions, receivers, tuner, instance, epoch, out);
}

}  // namespace asicen
