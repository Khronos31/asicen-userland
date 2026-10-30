// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_USERLAND_ASICEND_RECEIVER_RECORDS_H
#define ASICEN_USERLAND_ASICEND_RECEIVER_RECORDS_H

#include "asicen/product_profile.h"
#include "px4/ipc.h"

#include <array>
#include <cstdint>

namespace asicen::receiver_table {

inline px4::userland::Result<std::array<px4::userland::ipc::ReceiverRecord,
                                        px4::userland::ipc::kReceiverCount>>
receiver_records(std::uint8_t receiver_count) noexcept
{
    using namespace px4::userland;
    using namespace px4::userland::ipc;

    std::array<ReceiverRecord, kReceiverCount> records{};
    if (!profile::valid_receiver_count(receiver_count)) {
        return Result<std::array<ReceiverRecord, kReceiverCount>>::failure(
            Error::INVALID_ARGUMENT);
    }
    for (std::uint8_t index = 0U; index < receiver_count; ++index) {
        const std::uint8_t local = profile::local_receiver_for(index);
        const std::uint8_t device = static_cast<std::uint8_t>(
            profile::usb_function_for_receiver(index) + 1U);
        const System system = profile::is_combined_receiver(receiver_count, index)
            ? System::ISDB_T_OR_S
            : profile::is_satellite_receiver(index) ? System::ISDB_S : System::ISDB_T;
        records[index] = ReceiverRecord{index, device, local, system};
    }
    return Result<std::array<ReceiverRecord, kReceiverCount>>::success(records);
}

}  // namespace asicen::receiver_table

#endif  // ASICEN_USERLAND_ASICEND_RECEIVER_RECORDS_H
