// SPDX-License-Identifier: GPL-2.0-only
#include "px4d_list_format.h"

#include "px4/ipc.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace px4::userland::tools {
namespace {

void append_usb_id(std::string& out, std::uint16_t vid, std::uint16_t pid)
{
    char buffer[16]{};
    const int count = std::snprintf(buffer, sizeof(buffer), "%04x:%04x",
                                    static_cast<unsigned int>(vid),
                                    static_cast<unsigned int>(pid));
    if (count > 0) out.append(buffer, static_cast<std::size_t>(count));
}

void append_number(std::string& out, unsigned int number)
{
    out += std::to_string(number);
}

const char* boolean(bool value) noexcept { return value ? "true" : "false"; }

// Serial descriptors are untrusted. Keep text records on one line.
void append_text_value(std::string& out, std::string_view value)
{
    for (char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        out.push_back(byte > 0x20U && byte < 0x7fU ? character : '?');
    }
}

void append_json_string(std::string& out, std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (std::size_t i = 0U; i < value.size();) {
        const char character = value[i];
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            out.push_back('\\');
            out.push_back(character);
            ++i;
        } else if (byte < 0x20U) {
            out += "\\u00";
            out.push_back(hex[byte >> 4U]);
            out.push_back(hex[byte & 0x0fU]);
            ++i;
        } else if (byte < 0x80U) {
            out.push_back(character);
            ++i;
        } else {
            // libusb normally supplies UTF-8, but an invalid descriptor must
            // not turn the entire JSON document into invalid UTF-8.
            std::size_t width = 0U;
            if (byte >= 0xc2U && byte <= 0xdfU) width = 2U;
            else if (byte >= 0xe0U && byte <= 0xefU) width = 3U;
            else if (byte >= 0xf0U && byte <= 0xf4U) width = 4U;
            bool valid = width != 0U && i + width <= value.size();
            if (valid) {
                for (std::size_t j = 1U; j < width; ++j) {
                    const auto next = static_cast<unsigned char>(value[i + j]);
                    if (next < 0x80U || next > 0xbfU) valid = false;
                }
                const auto next = static_cast<unsigned char>(value[i + 1U]);
                if (byte == 0xe0U && next < 0xa0U) valid = false;
                if (byte == 0xedU && next > 0x9fU) valid = false;
                if (byte == 0xf0U && next < 0x90U) valid = false;
                if (byte == 0xf4U && next > 0x8fU) valid = false;
            }
            if (valid) {
                out.append(value.data() + i, width);
                i += width;
            } else {
                out += "\\ufffd";
                ++i;
            }
        }
    }
    out.push_back('"');
}

std::string port_string(const UsbLocation& location)
{
    if (!location.has_bus || location.port_count == 0U) return {};
    std::string value = std::to_string(location.bus);
    for (std::size_t i = 0U; i < location.port_count && i < location.port_path.size(); ++i) {
        value.push_back(i == 0U ? '-' : '.');
        append_number(value, location.port_path[i]);
    }
    return value;
}

void append_text_location(std::string& out, std::string_view prefix,
                          const UsbLocation& location)
{
    if (location.has_bus) {
        out.push_back(' ');
        out.append(prefix.data(), prefix.size());
        out += "bus=";
        append_number(out, location.bus);
    }
    if (location.has_address) {
        out.push_back(' ');
        out.append(prefix.data(), prefix.size());
        out += "address=";
        append_number(out, location.address);
    }
    const std::string port = port_string(location);
    if (!port.empty()) {
        out.push_back(' ');
        out.append(prefix.data(), prefix.size());
        out += "port=";
        out += port;
    }
}

void append_json_location(std::string& out, const UsbLocation& location)
{
    out += ",\"bus\":";
    if (location.has_bus) append_number(out, location.bus);
    else out += "null";
    out += ",\"address\":";
    if (location.has_address) append_number(out, location.address);
    else out += "null";
    out += ",\"port\":";
    const std::string port = port_string(location);
    if (port.empty()) out += "null";
    else append_json_string(out, port);
}

bool serial_unique(const GroupingResult& grouping, const Q3U4Group& group) noexcept
{
    if (group.status != GroupStatus::ready) return false;
    std::size_t matching_count = 0U;
    for (const Q3U4Group& other : grouping.groups) {
        if (other.base_serial == group.base_serial) ++matching_count;
    }
    return matching_count == 1U;
}

const char* system_name(ipc::System system) noexcept
{
    switch (system) {
    case ipc::System::ISDB_T: return "ISDB-T";
    case ipc::System::ISDB_S: return "ISDB-S";
    case ipc::System::ISDB_T_OR_S: return "ISDB-T/S";
    }
    return "";
}

bool lnb_supported(const DeviceProfile& profile, ipc::System system) noexcept
{
    return profile.supports_lnb_15v && system != ipc::System::ISDB_T;
}

void append_receiver_text(std::string& out, const ipc::ReceiverRecord& receiver,
                          const DeviceProfile& profile)
{
    out += "receiver=";
    append_number(out, receiver.global_id);
    out += " device=";
    append_number(out, receiver.dev_id);
    out += " local=";
    append_number(out, receiver.local_id);
    out += " system=";
    out += system_name(receiver.system);
    out += " lnb_15v_supported=";
    out += boolean(lnb_supported(profile, receiver.system));
    out.push_back('\n');
}

void append_receiver_json(std::string& out, const ipc::ReceiverRecord& receiver,
                          const DeviceProfile& profile)
{
    out += "{\"receiver\":";
    append_number(out, receiver.global_id);
    out += ",\"device\":";
    append_number(out, receiver.dev_id);
    out += ",\"local\":";
    append_number(out, receiver.local_id);
    out += ",\"system\":";
    append_json_string(out, system_name(receiver.system));
    out += ",\"lnb_15v_supported\":";
    out += boolean(lnb_supported(profile, receiver.system));
    out.push_back('}');
}

void append_json_observation(std::string& out, unsigned int device,
                             const DeviceObservation& observation)
{
    out += "{\"device\":";
    append_number(out, device);
    out += ",\"serial\":";
    append_json_string(out, observation.serial);
    append_json_location(out, observation.location);
    out.push_back('}');
}

} // namespace

std::string format_device_list(const GroupingResult& grouping)
{
    std::string out;
    for (const Q3U4Group& group : grouping.groups) {
        const DeviceProfile& profile = device_profile(group.model);
        out += "serial=";
        append_text_value(out, group.base_serial);
        out += " model=";
        out += profile.name;
        out += " usb=";
        append_usb_id(out, kQ3U4VendorId, profile.product_id);
        out += " status=";
        out += group_status_string(group.status);
        out += " receivers=";
        append_number(out, profile.receiver_count);
        out += " serial_unique=";
        out += boolean(serial_unique(grouping, group));
        if (group.status == GroupStatus::duplicate) {
            for (std::size_t i = 0U; i < group.candidates.size(); ++i) {
                const Q3U4Group::Candidate& candidate = group.candidates[i];
                const std::string prefix = "candidate" + std::to_string(i + 1U) + "_";
                out.push_back(' ');
                out += prefix;
                out += "device=";
                append_number(out, candidate.device);
                append_text_location(out, prefix, candidate.observation.location);
            }
        } else {
            for (std::size_t i = 0U; i < profile.bridge_count; ++i) {
                if (!group.devices[i]) continue;
                const std::string prefix = "dev" + std::to_string(i + 1U) + "_";
                append_text_location(out, prefix, group.devices[i]->location);
            }
        }
        out.push_back('\n');
        const auto records = ipc::receiver_records(profile.receiver_count, profile.dual_system);
        if (records) {
            for (std::size_t i = 0U; i < profile.receiver_count; ++i) {
                append_receiver_text(out, records.value()[i], profile);
            }
        }
    }
    for (const RejectedObservation& rejected : grouping.rejected) {
        const DeviceObservation& observation = rejected.observation;
        const DeviceProfile* profile =
            device_profile_for_usb_id(observation.vendor_id, observation.product_id);
        if (profile == nullptr) continue;
        out += "rejected serial=";
        append_text_value(out, observation.serial);
        out += " model=";
        out += profile->name;
        out += " usb=";
        append_usb_id(out, observation.vendor_id, observation.product_id);
        out += " status=";
        out += observation_status_string(rejected.status);
        append_text_location(out, "", observation.location);
        out.push_back('\n');
    }
    return out;
}

std::string format_device_list_json(const GroupingResult& grouping)
{
    std::string out{"{\"enclosures\":["};
    bool first_group = true;
    for (const Q3U4Group& group : grouping.groups) {
        if (!first_group) out.push_back(',');
        first_group = false;
        const DeviceProfile& profile = device_profile(group.model);
        out += "{\"serial\":";
        append_json_string(out, group.base_serial);
        out += ",\"model\":";
        append_json_string(out, profile.name);
        out += ",\"usb\":";
        std::string usb;
        append_usb_id(usb, kQ3U4VendorId, profile.product_id);
        append_json_string(out, usb);
        out += ",\"status\":";
        append_json_string(out, group_status_string(group.status));
        out += ",\"serial_unique\":";
        out += boolean(serial_unique(grouping, group));
        out += ",\"devices\":[";
        bool first_device = true;
        if (group.status != GroupStatus::duplicate) {
            for (std::size_t i = 0U; i < profile.bridge_count; ++i) {
                if (!group.devices[i]) continue;
                if (!first_device) out.push_back(',');
                first_device = false;
                append_json_observation(out, static_cast<unsigned int>(i + 1U), *group.devices[i]);
            }
        }
        out += "],\"candidates\":[";
        if (group.status == GroupStatus::duplicate) {
            for (std::size_t i = 0U; i < group.candidates.size(); ++i) {
                if (i != 0U) out.push_back(',');
                append_json_observation(out, group.candidates[i].device,
                                        group.candidates[i].observation);
            }
        }
        out += "],\"receivers\":[";
        const auto records = ipc::receiver_records(profile.receiver_count, profile.dual_system);
        if (records) {
            for (std::size_t i = 0U; i < profile.receiver_count; ++i) {
                if (i != 0U) out.push_back(',');
                append_receiver_json(out, records.value()[i], profile);
            }
        }
        out += "]}";
    }
    out += "],\"ungrouped_usb_devices\":[";
    bool first_rejected = true;
    for (const RejectedObservation& rejected : grouping.rejected) {
        const DeviceObservation& observation = rejected.observation;
        const DeviceProfile* profile =
            device_profile_for_usb_id(observation.vendor_id, observation.product_id);
        if (profile == nullptr) continue;
        if (!first_rejected) out.push_back(',');
        first_rejected = false;
        out += "{\"serial\":";
        if (observation.serial.empty() || rejected.status == ObservationStatus::open_failed)
            out += "null";
        else append_json_string(out, observation.serial);
        out += ",\"model\":";
        append_json_string(out, profile->name);
        out += ",\"usb\":";
        std::string usb;
        append_usb_id(usb, observation.vendor_id, observation.product_id);
        append_json_string(out, usb);
        out += ",\"status\":";
        append_json_string(out, observation_status_string(rejected.status));
        append_json_location(out, observation.location);
        out.push_back('}');
    }
    out += "]}\n";
    return out;
}

} // namespace px4::userland::tools
