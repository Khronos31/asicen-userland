#include "asicen/frontend_sequence.h"
#include "asicen/write_protocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(bool value, const char* name) {
    if (!value) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

class DistinctChunkTransport final : public asicen::FrontendTransport {
public:
    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override {
        transfers.push_back(transfer);
        for (std::uint16_t i = 0; i < transfer.length; ++i) {
            data[i] = 0;
        }
        if (transfer.length != 0) {
            data[0] = 1;
        }
        if (transfer.request == asicen::Request::ChannelFilterRead) {
            const std::size_t offset = transfer.value & 0xffU;
            const std::size_t payload = transfer.length - 1U;
            for (std::size_t i = 0; i < payload; ++i) {
                data[i + 1U] = static_cast<unsigned char>(offset + i + 1U);
            }
        }
        return transfer.length;
    }
    void delay_ms(unsigned) override {}

    std::vector<asicen::ControlTransfer> transfers;
};

class SparseChunkTransport final : public asicen::FrontendTransport {
public:
    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override {
        transfers.push_back(transfer);
        for (std::uint16_t i = 0; i < transfer.length; ++i) {
            data[i] = 0;
        }
        if (transfer.length != 0) {
            data[0] = 1;
        }
        if (transfer.request == asicen::Request::ChannelFilterRead) {
            const std::size_t offset = transfer.value & 0xffU;
            if (offset == 0x00) {
                data[7] = 0x31;
            }
            if (offset == 0x40) {
                data[1] = 0x42;
            }
        }
        return transfer.length;
    }
    void delay_ms(unsigned) override {}

    std::vector<asicen::ControlTransfer> transfers;
};

class ResetStateTransport final : public asicen::FrontendTransport {
public:
    ResetStateTransport() {
        for (std::size_t i = 0; i < block.size(); ++i)
            block[i] = static_cast<std::uint8_t>(i + 1U);
        block[0x40] = 0xa8;
    }
    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override {
        transfers.push_back(transfer);
        if (transfer.request == asicen::Request::ChannelFilterRead) {
            const std::size_t offset = transfer.value & 0xffU;
            const std::size_t size = transfer.length - 1U;
            data[0] = 1;
            std::copy_n(block.begin() + offset, size, data + 1);
        } else if (transfer.request == asicen::Request::ResetChannel) {
            data[0] = 1;
        } else if (transfer.request == asicen::Request::ChannelFilterWrite) {
            const std::size_t offset = transfer.value & 0xffU;
            const std::size_t size = transfer.length - 1U;
            block[offset] = static_cast<std::uint8_t>(transfer.value >> 8U);
            if (size > 1) block[offset + 1] = static_cast<std::uint8_t>(transfer.index);
            if (size > 2) block[offset + 2] = static_cast<std::uint8_t>(transfer.index >> 8U);
            data[0] = 1;
        }
        return transfer.length;
    }
    void delay_ms(unsigned) override {}
    std::array<std::uint8_t, 0x45> block{};
    std::vector<asicen::ControlTransfer> transfers;
};

std::vector<std::uint8_t> read_offsets(const std::vector<asicen::ControlTransfer>& transfers) {
    std::vector<std::uint8_t> offsets;
    for (const auto& transfer : transfers) {
        if (transfer.request == asicen::Request::ChannelFilterRead) {
            offsets.push_back(static_cast<std::uint8_t>(transfer.value & 0xffU));
        }
    }
    return offsets;
}

std::vector<std::uint8_t> write_offsets(const std::vector<asicen::ControlTransfer>& transfers) {
    std::vector<std::uint8_t> offsets;
    for (const auto& transfer : transfers) {
        if (transfer.request == asicen::Request::ChannelFilterWrite) {
            offsets.push_back(static_cast<std::uint8_t>(transfer.value & 0xffU));
        }
    }
    return offsets;
}

}  // namespace

int main() {
    for (std::size_t payload = 1; payload <= 3; ++payload) {
        check(asicen::cf_chunk_write_response_complete(
                  static_cast<int>(payload + 1U), payload),
              "CF full write transfer length includes status byte plus payload");
        check(!asicen::cf_chunk_write_response_complete(
                  static_cast<int>(payload), payload),
              "CF short chunk write is rejected by exact transfer length");
    }
    check(!asicen::cf_chunk_write_response_complete(4, 0),
          "empty CF write chunk is rejected");
    {
        std::array<std::uint8_t, 0x45> snapshot{};
        snapshot[0x40] = 0x08;
        const auto plan = asicen::build_cf_block_write_plan(1, snapshot.data(),
                                                            snapshot.size());
        check(plan.size() == 23,
              "full CF restore plan includes all chunks, including all-zero chunks");
        if (plan.size() == 23) {
            for (std::size_t i = 0; i < plan.size(); ++i) {
                check(plan[i].request == asicen::Request::ChannelFilterWrite &&
                          (plan[i].value & 0xffU) ==
                              static_cast<std::uint16_t>(0x80U + i * 3U) &&
                          plan[i].length == 4,
                      "full CF restore uses every three-byte lane-1 chunk and exact length");
            }
        }
        check(asicen::build_cf_block_write_plan(2, snapshot.data(), snapshot.size()).empty(),
              "full CF restore plan rejects invalid lane");
    }

    {
        ResetStateTransport transport;
        const auto original = transport.block;
        std::array<std::uint8_t, 0x45> before{};
        std::array<std::uint8_t, 0x45> after{};
        check(asicen::run_filter_reset_operation(&transport, 0, 1, &before, &after) ==
                  asicen::FrontendRunResult::Completed,
              "filter-repeat P operation executes one source filter reset");
        check(before == original && transport.block == after,
              "P exposes the exact full block read and written by reset");
        auto expected = original;
        expected[0x40] = static_cast<std::uint8_t>(expected[0x40] | 0x04U);
        check(after == expected && (after[0x40] & 0x08U) != 0,
              "reset-state 1 changes bit2 only and preserves bit3 and other bits");
    }
    const auto reset_plan = [] {
        asicen::FrontendOp op{};
        op.kind = asicen::FrontendOpKind::FilterReset;
        op.local = 0;
        op.flag = 1;
        op.block_rmw = true;
        asicen::FrontendPlan plan{op};
        return plan;
    };

    {
        const auto state0 = asicen::plan_stream_setup(0, 0);
        const auto state1 = asicen::plan_stream_setup(0, 1);
        check(state0.size() == 3 && state1.size() == 3,
              "explicit reset states preserve setup sequence");
        if (!state0.empty() && !state1.empty()) {
            check(state0[0].block_rmw && state1[0].block_rmw,
                  "third argument enables block read-modify-write");
            check(state0[0].flag == 0 && state1[0].flag == 1,
                  "fourth argument carries reset state independently");
        }
    }

    // Vendor USB_CF_Read advances by each chunk's length: 00,20,40.
    // Distinct response bytes also ensure each read's own payload is consumed.
    {
        DistinctChunkTransport transport;
        check(asicen::run_frontend_plan(reset_plan(), &transport) ==
                  asicen::FrontendRunResult::Completed,
              "distinct chunk reset completes");
        check(read_offsets(transport.transfers) == std::vector<std::uint8_t>({0x00, 0x20, 0x40}),
              "read offsets advance by actual chunk lengths");
        check(transport.transfers.size() == 27, "all nonzero write chunks preserved");
        const auto writes = write_offsets(transport.transfers);
        check(writes.size() == 23, "23 three-byte-or-short writes");
        if (writes.size() == 23) {
            for (std::size_t i = 0; i < writes.size(); ++i) {
                check(writes[i] == static_cast<std::uint8_t>(i * 3U),
                      "write offset advances by three");
            }
        }
    }

    // Skipped all-zero write chunks must not collapse subsequent addresses.
    {
        SparseChunkTransport transport;
        check(asicen::run_frontend_plan(reset_plan(), &transport) ==
                  asicen::FrontendRunResult::Completed,
              "sparse chunk reset completes");
        check(read_offsets(transport.transfers) == std::vector<std::uint8_t>({0x00, 0x20, 0x40}),
              "sparse fixture still reads three distinct ranges");
        const auto writes = write_offsets(transport.transfers);
        check(writes == std::vector<std::uint8_t>({0x06, 0x3f}),
              "zero chunks skipped without changing later write addresses");
    }

    return failures == 0 ? 0 : 1;
}
