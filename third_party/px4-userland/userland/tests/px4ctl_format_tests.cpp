// SPDX-License-Identifier: GPL-2.0-only
#include "px4ctl_format.h"

#include <array>
#include <cstdio>
#include <string>

namespace {

using namespace px4::userland;

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::fprintf(stderr, "px4ctl format CHECK failed at %s:%d: %s\n", __FILE__,     \
                         __LINE__, #condition);                                               \
            return false;                                                                    \
        }                                                                                    \
    } while (false)

bool test_list_does_not_invent_runtime_state()
{
    static constexpr std::array<std::uint8_t, 14U> serial{
        '0', '0', '0', '0', '1', '2', '0', '5', '0', '0', '0', '9', '6', '0'};
    ipc::ListResponsePayload payload{};
    payload.generation = 99U;
    payload.serial_utf8 = ByteView{serial.data(), serial.size()};
    payload.ready = 1U;
    payload.usb_present_mask = 3U;
    for (std::size_t receiver = 0U; receiver < payload.receivers.size(); ++receiver) {
        const std::uint8_t local = static_cast<std::uint8_t>(receiver % 4U);
        payload.receivers[receiver] = ipc::ReceiverRecord{
            static_cast<std::uint8_t>(receiver),
            static_cast<std::uint8_t>((receiver / 4U) + 1U), local,
            local < 2U ? ipc::System::ISDB_S : ipc::System::ISDB_T};
    }

    const std::string expected =
        "serial=00001205000960 ready=yes usb-present-mask=0x03\n"
        "receiver=0 device=1 local=0 system=ISDB-S\n"
        "receiver=1 device=1 local=1 system=ISDB-S\n"
        "receiver=2 device=1 local=2 system=ISDB-T\n"
        "receiver=3 device=1 local=3 system=ISDB-T\n"
        "receiver=4 device=2 local=0 system=ISDB-S\n"
        "receiver=5 device=2 local=1 system=ISDB-S\n"
        "receiver=6 device=2 local=2 system=ISDB-T\n"
        "receiver=7 device=2 local=3 system=ISDB-T\n";
    const std::string actual = tools::format_list(payload);
    CHECK(actual == expected);
    CHECK(actual.find("state=") == std::string::npos);
    return true;
}

bool test_status_prints_every_field_and_state_name()
{
    ipc::StatusResponsePayload payload{};
    payload.generation = UINT64_C(0x0102030405060708);
    payload.ready = 1U;
    payload.usb_present_mask = 2U;
    payload.card_present = 1U;
    payload.card_initialized = 0U;
    payload.receiver_states = {
        ipc::ReceiverState::free,
        ipc::ReceiverState::leased,
        ipc::ReceiverState::tuned,
        ipc::ReceiverState::streaming,
        ipc::ReceiverState::error,
        ipc::ReceiverState::free,
        ipc::ReceiverState::leased,
        ipc::ReceiverState::error,
    };
    payload.usb_errors = 123U;
    payload.protocol_errors = 456U;

    const std::string expected =
        "generation=72623859790382856 ready=yes usb-present-mask=0x02 "
        "card-present=yes card-initialized=no usb-errors=123 protocol-errors=456\n"
        "receiver=0 state=free\n"
        "receiver=1 state=leased\n"
        "receiver=2 state=tuned\n"
        "receiver=3 state=streaming\n"
        "receiver=4 state=error\n"
        "receiver=5 state=free\n"
        "receiver=6 state=leased\n"
        "receiver=7 state=error\n";
    CHECK(tools::format_status(payload) == expected);
    return true;
}

} // namespace

// v0.16: a five-receiver MLT5 enclosure lists only its receivers as
// dual-system and omits the absent STATUS slots.
bool test_mlt5pe_list_and_status()
{
    static constexpr std::array<std::uint8_t, 15U> serial{
        '0', '0', '0', '0', '2', '0', '2', '6', '3', '9', '0', '1', '4', '9', '1'};
    const auto records = ipc::receiver_records(ipc::kMlt5PeReceiverCount);
    if (!records) return false;
    const ipc::ListResponsePayload list{
        1U, ByteView{serial.data(), serial.size()}, 1U, 0x01U, records.value(),
        ipc::kMlt5PeReceiverCount};
    const std::string listed = tools::format_list(list);
    if (listed != "serial=000020263901491 ready=yes usb-present-mask=0x01\n"
                  "receiver=0 device=1 local=0 system=ISDB-T/S\n"
                  "receiver=1 device=1 local=1 system=ISDB-T/S\n"
                  "receiver=2 device=1 local=2 system=ISDB-T/S\n"
                  "receiver=3 device=1 local=3 system=ISDB-T/S\n"
                  "receiver=4 device=1 local=4 system=ISDB-T/S\n") {
        std::fprintf(stderr, "px4ctl mlt5pe list mismatch:\n%s", listed.c_str());
        return false;
    }

    ipc::StatusResponsePayload status{};
    status.usb_present_mask = 0x01U;
    status.receiver_states[4U] = ipc::ReceiverState::streaming;
    for (std::size_t index = 5U; index < ipc::kReceiverCount; ++index)
        status.receiver_states[index] = ipc::ReceiverState::absent;
    const std::string printed = tools::format_status(status);
    if (printed.find("receiver=4 state=streaming\n") == std::string::npos ||
        printed.find("receiver=5") != std::string::npos ||
        printed.find("absent") != std::string::npos) {
        std::fprintf(stderr, "px4ctl mlt5pe status mismatch:\n%s", printed.c_str());
        return false;
    }
    return true;
}

bool run_px4ctl_format_tests()
{
    return test_list_does_not_invent_runtime_state() &&
           test_status_prints_every_field_and_state_name() &&
           test_mlt5pe_list_and_status();
}
