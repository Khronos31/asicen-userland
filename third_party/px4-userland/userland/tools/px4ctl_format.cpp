// SPDX-License-Identifier: GPL-2.0-only
#include "px4ctl_format.h"

#include <cstdint>
#include <cstdio>

namespace px4::userland::tools {
namespace {

const char* yes_no(std::uint8_t value) noexcept
{
    return value != 0U ? "yes" : "no";
}

const char* system_name(ipc::System system) noexcept
{
    switch (system) {
    case ipc::System::ISDB_T:
        return "ISDB-T";
    case ipc::System::ISDB_S:
        return "ISDB-S";
    case ipc::System::ISDB_T_OR_S:
        return "ISDB-T/S";
    }
    return "";
}

const char* receiver_state_name(ipc::ReceiverState state) noexcept
{
    // The status decoder rejects values outside this closed enum before the
    // formatter is called.  Keep that validation at the wire boundary.
    switch (state) {
    case ipc::ReceiverState::free:
        return "free";
    case ipc::ReceiverState::leased:
        return "leased";
    case ipc::ReceiverState::tuned:
        return "tuned";
    case ipc::ReceiverState::streaming:
        return "streaming";
    case ipc::ReceiverState::error:
        return "error";
    case ipc::ReceiverState::absent:
        return "absent";
    }
    return "";
}

} // namespace

std::string format_list(const ipc::ListResponsePayload& payload)
{
    std::string output{"serial="};
    if (payload.serial_utf8.size != 0U) {
        output.append(reinterpret_cast<const char*>(payload.serial_utf8.data),
                      payload.serial_utf8.size);
    }

    char line[160]{};
    int length = std::snprintf(line, sizeof(line),
                               " ready=%s usb-present-mask=0x%02x\n",
                               yes_no(payload.ready),
                               static_cast<unsigned int>(payload.usb_present_mask));
    if (length > 0) output.append(line, static_cast<std::size_t>(length));

    output += format_receiver_records(payload.receivers.data(), payload.receiver_count);
    return output;
}

std::string format_receiver_records(const ipc::ReceiverRecord* records, std::size_t count)
{
    std::string output;
    char line[160]{};
    for (std::size_t index = 0U; index < count; ++index) {
        const ipc::ReceiverRecord& receiver = records[index];
        const int length = std::snprintf(line, sizeof(line),
                                         "receiver=%u device=%u local=%u system=%s\n",
                                         static_cast<unsigned int>(receiver.global_id),
                                         static_cast<unsigned int>(receiver.dev_id),
                                         static_cast<unsigned int>(receiver.local_id),
                                         system_name(receiver.system));
        if (length > 0) output.append(line, static_cast<std::size_t>(length));
    }
    return output;
}

std::string format_status(const ipc::StatusResponsePayload& payload)
{
    char line[256]{};
    int length = std::snprintf(
        line, sizeof(line),
        "generation=%llu ready=%s usb-present-mask=0x%02x "
        "card-present=%s card-initialized=%s usb-errors=%llu protocol-errors=%llu\n",
        static_cast<unsigned long long>(payload.generation), yes_no(payload.ready),
        static_cast<unsigned int>(payload.usb_present_mask), yes_no(payload.card_present),
        yes_no(payload.card_initialized),
        static_cast<unsigned long long>(payload.usb_errors),
        static_cast<unsigned long long>(payload.protocol_errors));

    std::string output;
    if (length > 0) output.append(line, static_cast<std::size_t>(length));
    for (std::size_t receiver = 0U; receiver < payload.receiver_states.size(); ++receiver) {
        // Slots beyond a smaller enclosure are not receivers.
        if (payload.receiver_states[receiver] == ipc::ReceiverState::absent) continue;
        length = std::snprintf(line, sizeof(line), "receiver=%u state=%s\n",
                               static_cast<unsigned int>(receiver),
                               receiver_state_name(payload.receiver_states[receiver]));
        if (length > 0) output.append(line, static_cast<std::size_t>(length));
    }
    return output;
}

} // namespace px4::userland::tools
