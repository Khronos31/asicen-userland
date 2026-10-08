#include "asicen/frontend_sequence.h"
#include "asicen/write_protocol.h"

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
