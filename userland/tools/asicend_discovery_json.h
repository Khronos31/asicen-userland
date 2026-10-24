// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_USERLAND_ASICEND_DISCOVERY_JSON_H
#define ASICEN_USERLAND_ASICEND_DISCOVERY_JSON_H

#include "asicen/device_profile.h"
#include "asicen/enclosure_grouping.h"
#include "px4/ipc.h"

#include <dtv/userland/discovery_json.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace asicen::cli {
namespace discovery_detail {

namespace wire = dtv::userland::wire;

template <std::size_t N>
wire::WString<N> text(const char* value)
{
    wire::WString<N> result;
    const std::size_t length = std::min(std::strlen(value), N);
    result.length = static_cast<std::uint16_t>(length);
    std::memcpy(result.bytes.data(), value, length);
    return result;
}

std::string port_string(const UsbFunctionObservation& observation)
{
    if (observation.bus == 0U || observation.port_path.empty()) return {};
    std::string value = std::to_string(observation.bus);
    for (std::size_t i = 0U; i < observation.port_path.size(); ++i) {
        value.push_back(i == 0U ? '-' : '.');
        value += std::to_string(observation.port_path[i]);
    }
    return value;
}

wire::DiscoveryUsbLocation location(const UsbFunctionObservation& observation)
{
    wire::DiscoveryUsbLocation result;
    if (observation.bus != 0U) {
        result.bus.present = true;
        result.bus.value = observation.bus;
    }
    if (observation.address != 0U) {
        result.address.present = true;
        result.address.value = observation.address;
    }
    const std::string port = port_string(observation);
    if (!port.empty()) {
        result.port.present = true;
        result.port.value = text<64U>(port.c_str());
    }
    return result;
}

bool is_loader(const UsbFunctionObservation& observation) noexcept
{
    return observation.vid == 0x1738U &&
        (observation.pid == 0x5211U || observation.pid == 0x5216U);
}

bool groupable(const UsbFunctionObservation& observation) noexcept
{
    return observation.parent_hub_known && observation.parent_hub_vid == 0x05e3U &&
        observation.parent_hub_pid == 0x0608U && observation.port_path.size() >= 2U &&
        (observation.port_path.back() == 1U || observation.port_path.back() == 2U);
}

bool same_enclosure_parent(const UsbFunctionObservation& a,
                           const UsbFunctionObservation& b) noexcept
{
    return groupable(a) && groupable(b) && a.bus == b.bus &&
        a.port_path.size() == b.port_path.size() &&
        std::equal(a.port_path.begin(), a.port_path.end() - 1, b.port_path.begin());
}

std::uint16_t device_id(const UsbFunctionObservation& observation,
                        const DeviceProfile* profile) noexcept
{
    if (profile != nullptr && profile->expected_runtime_functions == 1U) return 1U;
    if (groupable(observation)) return observation.port_path.back();
    return 1U;
}

struct EnclosureBuild final {
    std::vector<std::size_t> members;
    std::vector<wire::DiscoveryEnclosure> enclosures;
    std::vector<std::vector<wire::DiscoveryDevice>> devices;
    std::vector<std::vector<wire::DiscoveryReceiver>> receivers;
    std::vector<std::vector<wire::DiscoveryReader>> readers;
    std::vector<std::vector<wire::DiscoveryCandidate>> candidates;
};

void append_enclosure(EnclosureBuild& build,
                      const std::vector<UsbFunctionObservation>& observations,
                      bool duplicate)
{
    const std::size_t index = build.enclosures.size();
    std::vector<const DeviceProfile*> profiles;
    for (const std::size_t member : build.members) {
        const auto& observation = observations[member];
        const DeviceProfile* profile = find_profile(observation.vid, observation.pid);
        if (profile == nullptr) continue;
        if (std::find(profiles.begin(), profiles.end(), profile) == profiles.end())
            profiles.push_back(profile);
    }
    const DeviceProfile* profile = profiles.size() == 1U ? profiles.front() : nullptr;
    if (profile != nullptr && profile->expected_runtime_functions == 1U &&
        build.members.size() > 1U) {
        profile = nullptr;
    }

    wire::DiscoveryEnclosure enclosure;
    enclosure.enclosure = static_cast<std::uint16_t>(index + 1U);
    enclosure.status = duplicate ? wire::DiscoveryEnclosureStatus::duplicate
        : wire::DiscoveryEnclosureStatus::incomplete;
    if (profile != nullptr) {
        enclosure.model.present = true;
        enclosure.model.value = text<128U>(profile->model);
        enclosure.lnb_15v_supported.present = true;
        enclosure.lnb_15v_supported.value = profile->model_id == ModelId::W3u2 ||
            profile->model_id == ModelId::W3u3 || profile->model_id == ModelId::W3u3V2;
    }

    std::vector<wire::DiscoveryDevice> devices;
    std::vector<wire::DiscoveryCandidate> candidates;
    std::vector<wire::DiscoveryReader> readers;
    std::vector<std::size_t> receiver_offsets;
    bool ready = profile != nullptr && !duplicate &&
        build.members.size() == profile->expected_runtime_functions;
    bool function_one_observed = false;

    build.receivers.push_back({});

    for (const std::size_t member : build.members) {
        const auto& observation = observations[member];
        const DeviceProfile* observed_profile = find_profile(observation.vid, observation.pid);
        const std::uint16_t id = build.members.size() > 1U && groupable(observation)
            ? observation.port_path.back()
            : device_id(observation, observed_profile != nullptr ? observed_profile : profile);
        if (id == 1U) function_one_observed = true;
        if (duplicate) {
            wire::DiscoveryCandidate candidate;
            candidate.device = id;
            candidate.location = location(observation);
            candidates.push_back(candidate);
            continue;
        }

        wire::DiscoveryDevice device;
        device.device = id;
        device.vid = observation.vid;
        device.pid = observation.pid;
        device.location = location(observation);
        if (profile != nullptr && observed_profile == profile && !is_loader(observation) &&
            (profile->expected_runtime_functions == 1U || groupable(observation))) {
            const auto records = px4::userland::ipc::receiver_records(
                profile->enclosure_receiver_count, profile->combined_isdb_ts);
            if (records) {
                receiver_offsets.push_back(build.receivers.back().size());
                for (std::size_t i = 0U; i < profile->enclosure_receiver_count; ++i) {
                    const auto& record = records.value()[i];
                    if (record.dev_id != id) continue;
                    wire::DiscoveryReceiver receiver;
                    receiver.receiver = record.global_id;
                    receiver.device = id;
                    receiver.local = record.local_id;
                    receiver.systems.mask = record.system == px4::userland::ipc::System::ISDB_T
                        ? 0x01U : record.system == px4::userland::ipc::System::ISDB_S
                            ? 0x02U : 0x03U;
                    build.receivers.back().push_back(receiver);
                }
                device.receivers_known = true;
                device.receiver_count = build.receivers.back().size() - receiver_offsets.back();
            } else {
                receiver_offsets.push_back(build.receivers.back().size());
            }
        } else {
            receiver_offsets.push_back(build.receivers.back().size());
        }
        devices.push_back(device);
    }

    if (duplicate) {
        enclosure.candidate_count = candidates.size();
    } else {
        for (const std::size_t member : build.members) {
            const auto& observation = observations[member];
            const DeviceProfile* observed_profile = find_profile(observation.vid, observation.pid);
            if (observed_profile != profile || is_loader(observation)) ready = false;
        }
        if (profile != nullptr && profile->expected_runtime_functions == 2U &&
            build.members.size() == 2U) {
            bool first = false;
            bool second = false;
            for (const std::size_t member : build.members) {
                const auto& path = observations[member].port_path;
                if (!path.empty() && path.back() == 1U) first = true;
                if (!path.empty() && path.back() == 2U) second = true;
            }
            ready = ready && first && second;
        }
        if (profile != nullptr && profile->expected_runtime_functions == 1U)
            ready = ready && build.members.size() == 1U;
        enclosure.status = ready ? wire::DiscoveryEnclosureStatus::ready
                                 : wire::DiscoveryEnclosureStatus::incomplete;
        if (profile != nullptr && function_one_observed) {
            enclosure.readers_known = true;
            readers.push_back({0U, 1U});
            enclosure.reader_count = readers.size();
        }
        enclosure.device_count = devices.size();
    }

    build.enclosures.push_back(enclosure);
    build.devices.push_back(std::move(devices));
    build.readers.push_back(std::move(readers));
    build.candidates.push_back(std::move(candidates));
    for (std::size_t i = 0U; i < build.devices.back().size(); ++i) {
        auto& device = build.devices.back()[i];
        if (device.receivers_known && device.receiver_count != 0U) {
            device.receivers = build.receivers.back().data() + receiver_offsets[i];
        }
    }
    build.enclosures.back().devices = build.devices.back().data();
    build.enclosures.back().readers = build.readers.back().data();
    build.enclosures.back().candidates = build.candidates.back().data();
}

}  // namespace discovery_detail

inline std::string format_discovery_json(
    const std::vector<UsbFunctionObservation>& observations,
    bool complete = true)
{
    using namespace discovery_detail;
    EnclosureBuild build;
    std::vector<bool> used(observations.size(), false);
    std::vector<wire::DiscoveryUsbObservation> ungrouped;
    for (std::size_t i = 0U; i < observations.size(); ++i) {
        if (used[i]) continue;
        const auto& observation = observations[i];
        if (groupable(observation)) {
            build.members.clear();
            for (std::size_t j = i; j < observations.size(); ++j) {
                if (!used[j] && same_enclosure_parent(observation, observations[j])) {
                    build.members.push_back(j);
                    used[j] = true;
                }
            }
            bool first = false;
            bool second = false;
            bool duplicate = false;
            for (const std::size_t member : build.members) {
                const std::uint8_t port = observations[member].port_path.back();
                if (port == 1U) { if (first) duplicate = true; first = true; }
                if (port == 2U) { if (second) duplicate = true; second = true; }
            }
            duplicate = duplicate || build.members.size() > 2U;
            if (build.members.size() == 2U && first && second) {
                std::sort(build.members.begin(), build.members.end(), [&](std::size_t a, std::size_t b) {
                    return observations[a].port_path.back() < observations[b].port_path.back();
                });
            }
            append_enclosure(build, observations, duplicate);
            continue;
        }
        used[i] = true;
        const DeviceProfile* profile = find_profile(observation.vid, observation.pid);
        if (profile != nullptr) {
            build.members = {i};
            append_enclosure(build, observations, false);
        } else if (is_loader(observation)) {
            wire::DiscoveryUsbObservation value;
            value.vid = observation.vid;
            value.pid = observation.pid;
            value.status = wire::DiscoveryObservationStatus::unknown;
            value.location = location(observation);
            ungrouped.push_back(value);
        }
    }

    std::vector<wire::DiscoveryScanIssue> issues;
    if (!complete) {
        wire::DiscoveryScanIssue issue;
        issue.code = text<64U>("usb_observation_incomplete");
        issue.detail = text<256U>("one or more USB descriptors could not be read");
        issues.push_back(issue);
    }
    wire::DiscoveryScan scan;
    scan.complete = complete;
    scan.product = text<32U>("asicen");
    scan.version = text<64U>(ASICEN_PROJECT_VERSION);
    scan.issue_count = issues.size();
    scan.issues = issues.data();
    scan.enclosure_count = build.enclosures.size();
    scan.enclosures = build.enclosures.data();
    scan.ungrouped_usb_device_count = ungrouped.size();
    scan.ungrouped_usb_devices = ungrouped.data();
    const auto size = wire::discovery_json_size(scan);
    if (!size) return {};
    std::vector<std::uint8_t> output(size.value());
    const auto written = wire::encode_discovery_json(scan, {output.data(), output.size()});
    if (!written) return {};
    return std::string(reinterpret_cast<const char*>(output.data()), written.value());
}

}  // namespace asicen::cli

#endif  // ASICEN_USERLAND_ASICEND_DISCOVERY_JSON_H
