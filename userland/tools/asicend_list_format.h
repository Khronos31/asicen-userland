// SPDX-License-Identifier: GPL-2.0-only
// Adapted from px4-userland px4d_list_format.cpp at
// 1a1485d0c3e972e0a47be907edb67949564aa9a7. ASICEN identities use observed
// topology, never invented USB serials; receiver order remains S,T,S,T.
#ifndef ASICEN_USERLAND_ASICEND_LIST_FORMAT_H
#define ASICEN_USERLAND_ASICEND_LIST_FORMAT_H

#include "asicen/device_profile.h"
#include "asicen/enclosure_grouping.h"
#include "asicend_identity.h"
#include "px4/ipc.h"

#include <dtv/userland/json.h>
#include <dtv/userland/identity.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <random>
#include <string_view>
#include <cstdio>
#include <string>
#include <vector>

namespace asicen::cli {

inline const char* boolean(bool value) noexcept { return value ? "true" : "false"; }

inline void append_usb_id(std::string& out, std::uint16_t vid, std::uint16_t pid)
{
    char buffer[16]{};
    const int count = std::snprintf(buffer, sizeof(buffer), "%04x:%04x",
                                    static_cast<unsigned int>(vid),
                                    static_cast<unsigned int>(pid));
    if (count > 0) out.append(buffer, static_cast<std::size_t>(count));
}

inline void append_json_location(std::string& out, const UsbFunctionObservation& location)
{
    out += ",\"bus\":";
    out += location.bus != 0U ? std::to_string(location.bus) : "null";
    out += ",\"address\":";
    out += location.address != 0U ? std::to_string(location.address) : "null";
    out += ",\"port\":";
    const std::string port = port_string(location);
    out += port.empty() ? "null" : '"' + port + '"';
}

inline void append_text_location(std::string& out, const std::string& prefix,
                                  const UsbFunctionObservation& location)
{
    if (location.bus != 0U) out += " " + prefix + "bus=" + std::to_string(location.bus);
    if (location.address != 0U)
        out += " " + prefix + "address=" + std::to_string(location.address);
    const std::string port = port_string(location);
    if (!port.empty()) out += " " + prefix + "port=" + port;
}

inline void append_json_observation(std::string& out, unsigned int device,
                                     const UsbFunctionObservation& observation)
{
    out += "{\"device\":" + std::to_string(device) + ",\"serial\":null";
    append_json_location(out, observation);
    out.push_back('}');
}

inline void append_receivers(std::string& out, const DeviceProfile& profile, bool json)
{
    const auto records = px4::userland::ipc::receiver_records(
        profile.enclosure_receiver_count, profile.combined_isdb_ts);
    if (!records) return;
    const bool lnb_control = profile.model_id == ModelId::W3u2 ||
                             profile.model_id == ModelId::W3u3 ||
                             profile.model_id == ModelId::W3u3V2;
    for (std::size_t i = 0U; i < profile.enclosure_receiver_count; ++i) {
        const auto& receiver = records.value()[i];
        const char* system = receiver.system == px4::userland::ipc::System::ISDB_T
                                 ? "ISDB-T"
                                 : receiver.system == px4::userland::ipc::System::ISDB_S
                                       ? "ISDB-S" : "ISDB-T/S";
        const bool lnb = lnb_control && receiver.system != px4::userland::ipc::System::ISDB_T;
        if (json) {
            if (i != 0U) out.push_back(',');
            out += "{\"receiver\":" + std::to_string(receiver.global_id);
            out += ",\"device\":" + std::to_string(receiver.dev_id);
            out += ",\"local\":" + std::to_string(receiver.local_id);
            out += ",\"system\":\"";
            out += system;
            out += "\",\"lnb_15v_supported\":";
            out += boolean(lnb);
            out.push_back('}');
        } else {
            out += "receiver=" + std::to_string(receiver.global_id);
            out += " device=" + std::to_string(receiver.dev_id);
            out += " local=" + std::to_string(receiver.local_id);
            out += " system=";
            out += system;
            out += " lnb_15v_supported=";
            out += boolean(lnb);
            out.push_back('\n');
        }
    }
}

inline std::string format_device_list(const std::vector<UsbFunctionObservation>& observations,
                                       bool json)
{
    std::string out = json ? "{\"enclosures\":[" : "";
    bool first_group = true;
    std::vector<bool> used(observations.size(), false);
    for (std::size_t index = 0U; index < observations.size(); ++index) {
        if (used[index]) continue;
        const auto& observation = observations[index];
        const DeviceProfile* profile = find_profile(observation.vid, observation.pid);
        if (profile == nullptr) continue;
        const bool dual = profile->expected_runtime_functions == 2U;
        const std::string identity = topology_identity(observation, dual);
        if (identity.empty()) continue;
        std::vector<std::size_t> members;
        for (std::size_t candidate = index; candidate < observations.size(); ++candidate) {
            if (used[candidate]) continue;
            const auto& other = observations[candidate];
            if (other.vid == observation.vid && other.pid == observation.pid &&
                topology_identity(other, dual) == identity) {
                members.push_back(candidate);
                used[candidate] = true;
            }
        }
        std::sort(members.begin(), members.end(), [&](std::size_t a, std::size_t b) {
            return observations[a].port_path < observations[b].port_path;
        });
        const bool canonical = !dual || (members.size() == 2U &&
            observations[members[0]].port_path.back() == 1U &&
            observations[members[1]].port_path.back() == 2U);
        const bool duplicate = members.size() > profile->expected_runtime_functions ||
            (dual && members.size() == 2U && !canonical);
        const char* status = duplicate ? "duplicate" :
            members.size() == profile->expected_runtime_functions && canonical ? "ready" : "incomplete";
        std::string usb;
        append_usb_id(usb, profile->vid, profile->pid);
        if (json) {
            if (!first_group) out.push_back(',');
            first_group = false;
            out += "{\"serial\":null,\"model\":\"";
            out += profile->model;
            out += "\",\"usb\":\"" + usb + "\",\"status\":\"" + status;
            out += "\",\"serial_unique\":false,\"identity_kind\":\"usb-topology\",\"identity\":\"";
            out += identity + "\",\"devices\":[";
            if (!duplicate) {
                for (std::size_t i = 0U; i < members.size(); ++i) {
                    if (i != 0U) out.push_back(',');
                    const auto& member = observations[members[i]];
                    append_json_observation(out, dual ? member.port_path.back() : 1U, member);
                }
            }
            out += "],\"candidates\":[";
            if (duplicate) {
                for (std::size_t i = 0U; i < members.size(); ++i) {
                    if (i != 0U) out.push_back(',');
                    const auto& member = observations[members[i]];
                    append_json_observation(out, dual ? member.port_path.back() : 1U, member);
                }
            }
            out += "],\"receivers\":[";
            append_receivers(out, *profile, true);
            out += "]}";
        } else {
            out += "serial= model=";
            out += profile->model;
            out += " usb=" + usb + " status=" + status;
            out += " receivers=" + std::to_string(profile->enclosure_receiver_count);
            out += " serial_unique=false identity_kind=usb-topology identity=" + identity;
            for (std::size_t i = 0U; i < members.size(); ++i) {
                const auto& member = observations[members[i]];
                const unsigned int device = dual ? member.port_path.back() : 1U;
                const std::string prefix = duplicate ? "candidate" + std::to_string(i + 1U) + "_"
                                                     : "dev" + std::to_string(device) + "_";
                if (duplicate) out += " " + prefix + "device=" + std::to_string(device);
                append_text_location(out, prefix, member);
            }
            out.push_back('\n');
            append_receivers(out, *profile, false);
        }
    }
    if (json) out += "],\"ungrouped_usb_devices\":[";
    bool first_rejected = true;
    for (std::size_t i = 0U; i < observations.size(); ++i) {
        if (used[i]) continue;
        const auto& observation = observations[i];
        const DeviceProfile* profile = find_profile(observation.vid, observation.pid);
        const char* model = profile != nullptr ? profile->model : "ASICEN firmware loader";
        const char* status = profile != nullptr ? "topology_unavailable" : "loader";
        std::string usb;
        append_usb_id(usb, observation.vid, observation.pid);
        if (json) {
            if (!first_rejected) out.push_back(',');
            first_rejected = false;
            out += "{\"serial\":null,\"model\":\"";
            out += model;
            out += "\",\"usb\":\"" + usb + "\",\"status\":\"" + status + '"';
            append_json_location(out, observation);
            out.push_back('}');
        } else {
            out += "rejected serial= model=";
            out += model;
            out += " usb=" + usb + " status=" + status;
            append_text_location(out, "", observation);
            out.push_back('\n');
        }
    }
    if (json) out += "]}\n";
    return out;
}


// Read-only userland.discovery v1.0 mapping. All claimed hardware facts come
// from the observed USB functions and existing static receiver profiles.
namespace shared = dtv::userland::wire;

template <std::size_t N>
inline bool discovery_text(shared::WString<N>& dest, std::string_view text)
{
    if (text.empty() || text.size() > dest.bytes.size()) return false;
    dest.length = static_cast<std::uint16_t>(text.size());
    std::memcpy(dest.bytes.data(), text.data(), text.size());
    return true;
}

inline std::uint8_t discovery_system(px4::userland::ipc::System system) noexcept
{
    switch (system) {
    case px4::userland::ipc::System::ISDB_T: return 1U;
    case px4::userland::ipc::System::ISDB_S: return 2U;
    case px4::userland::ipc::System::ISDB_T_OR_S: return 3U;
    }
    return 0U;
}

inline bool discovery_selector(shared::DiscoveryFunction& target,
                               const UsbFunctionObservation& source,
                               shared::DiscoveryCandidateState state)
{
    const std::string port = port_string(source);
    if (port.empty() ||
        !dtv::userland::identity::parse_device_selector("usb:" + port))
        return true; // Position was not observed; do not fabricate a selector.
    if (target.selector_candidates.count == target.selector_candidates.items.size())
        return false;
    target.observation.location.usb_path.present =
        discovery_text(target.observation.location.usb_path.value, port);
    auto& item = target.selector_candidates.items[target.selector_candidates.count++];
    item.state = state;
    return discovery_text(item.value, "usb:" + port);
}

inline std::string format_discovery_json(
    const std::vector<UsbFunctionObservation>& observations, const char* version)
{
    if (version == nullptr ||
        observations.size() > std::numeric_limits<std::uint16_t>::max()) return {};
    shared::DiscoveryResponse document{};
    if (!discovery_text(document.producer.product, "asicen-userland") ||
        !discovery_text(document.producer.version, version)) return {};

    std::vector<shared::DiscoveryFunction> functions(observations.size());
    std::vector<shared::DiscoveryEnclosure> enclosures;
    enclosures.reserve(observations.size());
    for (std::size_t i = 0U; i < observations.size(); ++i) {
        const auto& source = observations[i];
        const auto* profile = find_profile(source.vid, source.pid);
        const bool loader = source.vid == 0x1738U &&
                            (source.pid == 0x5211U || source.pid == 0x5216U);
        auto& target = functions[i];
        auto& usb = target.observation;
        usb.id = static_cast<std::uint16_t>(i);
        usb.vid = source.vid;
        usb.pid = source.pid;
        usb.mode = loader ? shared::Mode::cold :
                   profile == nullptr ? shared::Mode::unknown : shared::Mode::runtime;
        usb.identification_reason = loader ? shared::IdentificationReason::cold_model :
            profile == nullptr ? shared::IdentificationReason::model_unconfirmed :
                                 shared::IdentificationReason::none;
        usb.present = true;
        usb.serial_state = shared::SerialState::not_queried;
        if (source.bus != 0U) {
            usb.location.bus.present = true;
            usb.location.bus.value = source.bus;
        }
        if (source.address != 0U) {
            usb.location.address.present = true;
            usb.location.address.value = source.address;
        }
        if (profile != nullptr) {
            usb.model.present = true;
            if (!discovery_text(usb.model.value.id, profile->model) ||
                !discovery_text(usb.model.value.name, profile->model)) return {};
        }
        target.grouping = shared::DiscoveryGroupingState::unresolved;
        target.grouping_reason.present = discovery_text(target.grouping_reason.value,
                                                        loader ? "cold_model" : "not_bound");
        if (!discovery_selector(target, source,
              shared::DiscoveryCandidateState::requires_resolution)) return {};
    }

    std::vector<bool> used(observations.size(), false);

    // A cold ASICEN USB function can be grouped by physical parent topology
    // before the shared loader reveals the runtime model.
    auto eligible_hub_member = [&](std::size_t index) {
        const auto& o = observations[index];
        if (!o.parent_genesys_hub || o.port_path.size() < 2U ||
            (o.port_path.back() != 1U && o.port_path.back() != 2U)) return false;
        const auto* p = find_profile(o.vid, o.pid);
        return is_loader_id(o.vid, o.pid) ||
               (p != nullptr && p->expected_runtime_functions == 2U);
    };
    for (std::size_t index = 0U; index < observations.size(); ++index) {
        if (used[index] || !eligible_hub_member(index)) continue;
        std::array<std::size_t, 2U> sibling_indices{};
        sibling_indices.fill(observations.size());
        bool any_loader = false;
        const auto& anchor = observations[index];
        for (std::size_t j = index; j < observations.size(); ++j) {
            if (used[j] || !eligible_hub_member(j)) continue;
            const auto& candidate = observations[j];
            if (candidate.bus != anchor.bus ||
                candidate.port_path.size() != anchor.port_path.size() ||
                !std::equal(candidate.port_path.begin(), candidate.port_path.end() - 1,
                            anchor.port_path.begin())) continue;
            sibling_indices[candidate.port_path.back() - 1U] = j;
            any_loader |= is_loader_id(candidate.vid, candidate.pid);
        }
        // Warm-only is handled below; cold or mixed has unknown composition.
        if (!any_loader) continue;
        if (enclosures.size() >= std::numeric_limits<std::uint16_t>::max()) return {};
        shared::DiscoveryEnclosure enclosure{};
        enclosure.descriptor.id = static_cast<std::uint16_t>(enclosures.size());
        enclosure.descriptor.identity.serial_state = shared::IdentitySerialState::unknown;
        enclosure.descriptor.identity.uniqueness = shared::Uniqueness::unknown;
        enclosure.topology.state = shared::TopologyState::unknown;
        enclosure.topology.expected_function_count = {true, 2U};
        for (std::size_t slot = 0U; slot < sibling_indices.size(); ++slot) {
            const auto j = sibling_indices[slot];
            if (j == observations.size()) continue;
            used[j] = true;
            auto& f = functions[j];
            f.grouping = shared::DiscoveryGroupingState::bound;
            f.grouping_reason.present = false;
            f.enclosure_id = {true, enclosure.descriptor.id};
            for (std::size_t k = 0; k < f.selector_candidates.count; ++k)
                enclosure.selector_candidates.items[enclosure.selector_candidates.count++] =
                    f.selector_candidates.items[k];
            enclosure.usb_function_ids.items[enclosure.usb_function_ids.count++] =
                f.observation.id;
        }
        // No invented reader/receiver composition in cold mode.
        enclosures.push_back(std::move(enclosure));
    }

    for (std::size_t index = 0U; index < observations.size(); ++index) {
        if (used[index]) continue;
        const auto* profile = find_profile(observations[index].vid,
                                           observations[index].pid);
        if (profile == nullptr) continue; // Cold loader remains unresolved.
        const bool dual = profile->expected_runtime_functions == 2U;
        if (dual && !eligible_hub_member(index)) continue;
        const std::string identity = topology_identity(observations[index], dual);
        if (identity.empty()) continue; // No stable observed topology.
        std::vector<std::size_t> members;
        for (std::size_t candidate = index; candidate < observations.size(); ++candidate) {
            if (used[candidate]) continue;
            const auto& other = observations[candidate];
            if (other.vid == observations[index].vid &&
                other.pid == observations[index].pid &&
                (!dual || eligible_hub_member(candidate)) &&
                topology_identity(other, dual) == identity) {
                members.push_back(candidate);
                used[candidate] = true;
            }
        }
        std::sort(members.begin(), members.end(),
                  [&](std::size_t left, std::size_t right) {
            return observations[left].port_path < observations[right].port_path;
        });
        const bool canonical = !dual || (members.size() == 2U &&
             !observations[members[0]].port_path.empty() &&
             !observations[members[1]].port_path.empty() &&
             observations[members[0]].port_path.back() == 1U &&
             observations[members[1]].port_path.back() == 2U);
        const bool duplicate = members.size() > profile->expected_runtime_functions ||
                               (dual && members.size() == 2U && !canonical);
        if (duplicate) {
            for (std::size_t n : members) {
                auto& f = functions[n];
                f.grouping = shared::DiscoveryGroupingState::ambiguous;
                f.grouping_reason.present = discovery_text(
                    f.grouping_reason.value, "duplicate_or_ambiguous");
                for (std::size_t j = 0U; j < f.selector_candidates.count; ++j)
                    f.selector_candidates.items[j].state =
                        shared::DiscoveryCandidateState::ambiguous;
            }
            continue;
        }
        // A member with an unrecognised slot cannot be assigned a receiver.
        // Keep its USB observation, but do not infer a missing sibling.
        std::array<std::uint16_t, 2U> slot_ids{};
        slot_ids.fill(std::numeric_limits<std::uint16_t>::max());
        for (std::size_t n : members) {
            const auto& m = observations[n];
            const std::size_t slot = dual ? (m.port_path.empty() ? 2U :
                                      static_cast<std::size_t>(m.port_path.back() - 1U)) : 0U;
            if (slot >= profile->expected_runtime_functions) continue;
            slot_ids[slot] = static_cast<std::uint16_t>(n);
        }
        std::size_t available = 0U;
        for (std::size_t slot = 0U; slot < profile->expected_runtime_functions; ++slot)
            if (slot_ids[slot] != std::numeric_limits<std::uint16_t>::max())
                ++available;
        if (available == 0U) continue;
        if (enclosures.size() >= std::numeric_limits<std::uint16_t>::max()) return {};

        shared::DiscoveryEnclosure enclosure{};
        enclosure.descriptor.id = static_cast<std::uint16_t>(enclosures.size());
        enclosure.descriptor.model.present = true;
        if (!discovery_text(enclosure.descriptor.model.value.id, profile->model) ||
            !discovery_text(enclosure.descriptor.model.value.name, profile->model)) return {};
        enclosure.descriptor.identity.serial_state = shared::IdentitySerialState::absent;
        enclosure.descriptor.identity.uniqueness = shared::Uniqueness::not_applicable;
        const bool complete = canonical && available == profile->expected_runtime_functions;
        enclosure.topology.state = complete ? shared::TopologyState::complete
                                            : shared::TopologyState::unknown;
        enclosure.topology.expected_function_count.present = true;
        enclosure.topology.expected_function_count.value = profile->expected_runtime_functions;
        for (std::size_t slot = 0U; slot < profile->expected_runtime_functions; ++slot) {
            const auto id = slot_ids[slot];
            if (id == std::numeric_limits<std::uint16_t>::max()) continue;
            auto& f = functions[id];
            f.grouping = shared::DiscoveryGroupingState::bound;
            f.grouping_reason.present = false;
            f.enclosure_id.present = true;
            f.enclosure_id.value = enclosure.descriptor.id;
            for (std::size_t j = 0U; j < f.selector_candidates.count; ++j) {
                f.selector_candidates.items[j].state =
                    shared::DiscoveryCandidateState::usable;
                enclosure.selector_candidates.items[enclosure.selector_candidates.count++] =
                    f.selector_candidates.items[j];
            }
            enclosure.usb_function_ids.items[enclosure.usb_function_ids.count++] = id;
        }
        if (!complete) {
            enclosures.push_back(std::move(enclosure));
            continue;
        }
        const auto records = px4::userland::ipc::receiver_records(
            profile->enclosure_receiver_count, profile->combined_isdb_ts);
        if (!records) return {};
        enclosure.receivers.present = true;
        enclosure.receivers.value.count = profile->enclosure_receiver_count;
        enclosure.descriptor.limits.max_concurrent_streams.present = true;
        enclosure.descriptor.limits.max_concurrent_streams.value =
            profile->enclosure_receiver_count;
        const bool lnb_supported = profile->model_id == ModelId::W3u2 ||
                                   profile->model_id == ModelId::W3u3 ||
                                   profile->model_id == ModelId::W3u3V2;
        for (std::size_t i = 0U; i < profile->enclosure_receiver_count; ++i) {
            const auto& src = records.value()[i];
            auto& dst = enclosure.receivers.value.items[i];
            dst.id = src.global_id;
            dst.systems.mask = discovery_system(src.system);
            const auto slot = static_cast<std::size_t>(src.dev_id - 1U);
            if (slot < profile->expected_runtime_functions &&
                slot_ids[slot] != std::numeric_limits<std::uint16_t>::max()) {
                dst.binding.present = true;
                dst.binding.value.function_id = slot_ids[slot];
                dst.binding.value.local_id = src.local_id;
            }
            if (lnb_supported && src.system != px4::userland::ipc::System::ISDB_T) {
                dst.capabilities.controls |= shared::kControlBitLnb;
                dst.capabilities.lnb.values.count = 2U;
                dst.capabilities.lnb.values.items[0] = 0U;
                dst.capabilities.lnb.values.items[1] = 15U;
            }
        }
        enclosure.readers.present = true;
        enclosure.readers.value.count = 1U;
        enclosure.readers.value.items[0].id = 0U;
        if (slot_ids[0] != std::numeric_limits<std::uint16_t>::max()) {
            enclosure.readers.value.items[0].binding.present = true;
            enclosure.readers.value.items[0].binding.value.function_id = slot_ids[0];
            enclosure.readers.value.items[0].binding.value.local_id = 0U;
        }
        enclosures.push_back(std::move(enclosure));
    }

    std::random_device source;
    for (auto& byte : document.scan_id)
        byte = static_cast<std::uint8_t>(source());
    document.usb_functions = {functions.data(), functions.size()};
    document.enclosures = {enclosures.data(), enclosures.size()};
    const auto size = shared::discovery_json_size(document);
    if (!size) return {};
    std::string result(size.value(), '\0');
    const auto encoded = shared::encode_discovery_json(document,
        {reinterpret_cast<std::uint8_t*>(result.data()), result.size()});
    if (!encoded || encoded.value() != result.size()) return {};
    return result;
}

}  // namespace asicen::cli

#endif  // ASICEN_USERLAND_ASICEND_LIST_FORMAT_H
