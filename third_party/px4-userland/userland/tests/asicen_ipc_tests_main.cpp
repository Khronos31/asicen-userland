// SPDX-License-Identifier: GPL-2.0-only
#include <cstdio>
#include "px4/ipc.h"

#include <array>

bool run_ipc_tests();

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;

bool test_product_topology_and_absent_serial()
{
    for (const std::uint8_t count : {0U, 3U, 5U, 7U, 8U, 255U}) {
        if (receiver_records(count).error() != Error::INVALID_ARGUMENT) return false;
    }
    for (const std::uint8_t count : {1U, 2U, 4U}) {
        const auto records = receiver_records(count);
        if (!records) return false;
        for (std::uint8_t index = 0U; index < count; ++index) {
            const ReceiverRecord& record = records.value()[index];
            if (record.global_id != index || record.dev_id != index / 2U + 1U ||
                record.local_id != index % 2U ||
                record.system != (count == 1U ? System::ISDB_T_OR_S :
                                  index % 2U == 0U ? System::ISDB_S : System::ISDB_T)) {
                return false;
            }
        }
        const ListResponsePayload value{1U, ByteView{nullptr, 0U}, 1U,
            static_cast<std::uint8_t>(count == 4U ? 3U : 1U), records.value(), count};
        std::array<std::uint8_t, 128U> output{};
        const auto encoded = encode_payload(value, MutableByteView{output.data(), output.size()});
        if (!encoded) return false;
        const auto decoded = decode_list_response_payload(ByteView{output.data(), encoded.value()});
        if (!decoded || decoded.value().serial_utf8.size != 0U ||
            decoded.value().receiver_count != count) return false;
    }
    return true;
}

}  // namespace

int main()
{
    if (!run_ipc_tests()) {
        std::fprintf(stderr, "FAIL ipc_wire_codec\n");
        return 1;
    }
    std::printf("PASS ipc_wire_codec\n");
    if (!test_product_topology_and_absent_serial()) {
        std::fprintf(stderr, "FAIL product_topology_and_absent_serial\n");
        return 1;
    }
    std::printf("PASS product_topology_and_absent_serial\n");
    return 0;
}
