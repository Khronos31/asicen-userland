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

#include <algorithm>
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

}  // namespace asicen::cli

#endif  // ASICEN_USERLAND_ASICEND_LIST_FORMAT_H
