// SPDX-License-Identifier: GPL-2.0-or-later

#include "asicen/satellite_tune.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <vector>

#include "asicen/write_protocol.h"

namespace {

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition);           \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

class SatelliteTransport final : public asicen::FrontendTransport {
  public:
    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override
    {
        const std::size_t call = transfers.size();
        transfers.push_back(transfer);
        if (call == fail_at)
            return -1;
        for (std::uint16_t i = 0; i < transfer.length; ++i)
            data[i] = 0;
        if (transfer.length > 0)
            data[0] = call == nack_at ? 0 : 1;
        if (transfer.request == asicen::Request::I2cRead && transfer.length > 1 &&
            call != nack_at) {
            const std::uint8_t reg = static_cast<std::uint8_t>(transfer.value >> 8U);
            if (reg == 0xc3) {
                data[1] = lock_reads++ < lock_reads_before ? 0x10U : lock_register;
            } else if (reg == 0xce) {
                for (std::size_t i = 0; i < tsids.size(); ++i) {
                    data[1 + i * 2] = static_cast<unsigned char>(tsids[i] >> 8U);
                    data[2 + i * 2] = static_cast<unsigned char>(tsids[i] & 0xffU);
                }
            } else if (reg == 0x8f) {
                data[1] = static_cast<unsigned char>(readback_tsid >> 8U);
                data[2] = static_cast<unsigned char>(readback_tsid & 0xffU);
            }
        }
        if (call == short_at)
            return static_cast<int>(transfer.length) - 1;
        return static_cast<int>(transfer.length);
    }

    void delay_ms(unsigned ms) override
    {
        delays.push_back(ms);
    }
    bool cancelled() const override
    {
        return cancel || transfers.size() >= cancel_after;
    }
    bool expired() const override
    {
        return expire;
    }

    std::vector<asicen::ControlTransfer> transfers;
    std::vector<unsigned> delays;
    std::size_t fail_at = std::numeric_limits<std::size_t>::max();
    std::size_t short_at = std::numeric_limits<std::size_t>::max();
    std::size_t nack_at = std::numeric_limits<std::size_t>::max();
    std::size_t cancel_after = std::numeric_limits<std::size_t>::max();
    std::uint8_t lock_register = 0x10;
    std::size_t lock_reads_before = 0;
    std::size_t lock_reads = 0;
    std::uint16_t readback_tsid = 0x1234;
    std::array<std::uint16_t, asicen::kW3u3SatelliteTsidSlots> tsids{
        0x1020, 0x3040, 0x5060, 0x7080, 0x90a0, 0xb0c0, 0xd0e0, 0x1234,
    };
    bool cancel = false;
    bool expire = false;
};

bool staged_tuner_messages(const asicen::FrontendPlan& plan,
                           std::vector<std::vector<std::uint8_t>>& messages)
{
    messages.clear();
    std::vector<std::uint8_t> staged;
    for (const auto& op : plan) {
        if (op.kind != asicen::FrontendOpKind::Control)
            continue;
        const auto& transfer = op.transfer;
        if (transfer.request == asicen::Request::I2cBufferFill) {
            const std::size_t count = transfer.length - 1U;
            staged.push_back(static_cast<std::uint8_t>(transfer.value >> 8U));
            if (count > 1)
                staged.push_back(static_cast<std::uint8_t>(transfer.index));
            if (count > 2)
                staged.push_back(static_cast<std::uint8_t>(transfer.index >> 8U));
        } else if (transfer.request == asicen::Request::I2cBufferSend) {
            CHECK(check(staged.size() + 1U == transfer.length,
                        "staged payload length matches send transfer"));
            CHECK(check(transfer.value == 0x0032, "satellite mode2 send uses slave32 and no-stop"));
            messages.push_back(staged);
            staged.clear();
        }
    }
    CHECK(check(staged.empty(), "all staged tuner bytes are sent"));
    return true;
}

bool expect_messages(std::uint32_t rf, const std::array<std::vector<std::uint8_t>, 4>& expected,
                     const char* name)
{
    const auto plan = asicen::plan_w3u3_satellite_tune(rf);
    std::vector<std::vector<std::uint8_t>> actual;
    CHECK(staged_tuner_messages(plan, actual));
    CHECK(check(actual.size() == expected.size(), name));
    if (actual.size() == expected.size()) {
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(check(actual[i] == expected[i], name));
        }
    }
    return true;
}

bool test_rf_vectors_and_tune_plan()
{
    for (std::size_t i = 0; i < 24; ++i) {
        const std::uint32_t rf = i < 12 ? 11727480U + static_cast<std::uint32_t>(i) * 38360U
                                        : 12291000U + static_cast<std::uint32_t>(i - 12) * 40000U;
        CHECK(check(asicen::is_w3u3_satellite_rf_khz(rf), "all 24 source RF rows supported"));
        CHECK(check(!asicen::plan_w3u3_satellite_tune(rf).empty(),
                    "all supported RF rows produce a plan"));
    }
    CHECK(check(!asicen::is_w3u3_satellite_rf_khz(11727481U), "non-table RF rejected"));
    CHECK(check(asicen::plan_w3u3_satellite_tune(11727481U).empty(), "invalid RF has no plan"));
    CHECK(check(asicen::plan_w3u3_satellite_tune(12291000U, 0x1234).empty(),
                "CS row rejects a prior BS TSID"));
    SatelliteTransport invalid_cs_tsid;
    CHECK(check(asicen::run_w3u3_satellite_tune(&invalid_cs_tsid, 12291000U, 0x1234) ==
                    asicen::SatelliteOperationResult::InvalidArgument,
                "CS row rejects invalid initial TSID through execution API"));
    CHECK(check(invalid_cs_tsid.transfers.empty(), "invalid CS TSID rejected before I/O"));

    CHECK(expect_messages(11727480U,
                          {{{0xfe, 0xc0, 0x28, 0x29, 0xe0, 0xd2},
                            {0xfe, 0xc0, 0xe4},
                            {0xfe, 0xc0, 0xf4, 0xd6},
                            {0xfe, 0xc0, 0x48}}},
                          "BS01 tuner message vector"));
    CHECK(expect_messages(12149440U,
                          {{{0xfe, 0xc0, 0x25, 0xbf, 0xe0, 0x60},
                            {0xfe, 0xc0, 0xe4},
                            {0xfe, 0xc0, 0xf4, 0x64},
                            {0xfe, 0xc0, 0x45}}},
                          "BS23 tuner message vector"));
    CHECK(expect_messages(12291000U,
                          {{{0xfe, 0xc0, 0x26, 0x4d, 0xe0, 0x60},
                            {0xfe, 0xc0, 0xe4},
                            {0xfe, 0xc0, 0xf4, 0x64},
                            {0xfe, 0xc0, 0x46}}},
                          "CS02 tuner message vector"));
    CHECK(expect_messages(12731000U,
                          {{{0xfe, 0xc0, 0x28, 0x05, 0xe0, 0xc0},
                            {0xfe, 0xc0, 0xe4},
                            {0xfe, 0xc0, 0xf4, 0xc4},
                            {0xfe, 0xc0, 0x48}}},
                          "CS24 tuner message vector"));

    const auto plan = asicen::plan_w3u3_satellite_tune(11727480U, 0x1234);
    std::vector<asicen::ControlTransfer> controls;
    std::vector<unsigned> delays;
    for (const auto& op : plan) {
        if (op.kind == asicen::FrontendOpKind::Control)
            controls.push_back(op.transfer);
        if (op.kind == asicen::FrontendOpKind::Delay)
            delays.push_back(op.delay_ms);
    }
    CHECK(check(controls.size() == 17, "tune has exact source-derived control count"));
    if (controls.size() == 17) {
        CHECK(check(controls[0].request == asicen::Request::I2cWrite &&
                        controls[0].value == 0x2532 && controls[0].index == 0x0000,
                    "tune begins with satellite demod 25=00"));
        CHECK(check(controls[1].value == 0x2332 && controls[1].index == 0x004d,
                    "tune writes satellite demod 23=4d"));
        CHECK(check(controls[2].value == 0x8f32 && controls[2].index == 0x0012,
                    "tune writes initial TSID high byte first"));
        CHECK(check(controls[3].value == 0x9032 && controls[3].index == 0x0034,
                    "initial TSID second byte advances from register 8f"));
        CHECK(check(controls[14].value == 0x0f30 && controls[14].index == 0x0034,
                    "satellite path finalizes adapter at slave30"));
        CHECK(check(controls[15].value == 0x0332 && controls[15].index == 0x0001,
                    "satellite path reacquires demod"));
        CHECK(check(controls[16].value == 0x2332 && controls[16].index == 0x004c,
                    "satellite path finishes with demod 23=4c"));
    }
    const auto cs_plan = asicen::plan_w3u3_satellite_tune(12291000U);
    std::vector<asicen::ControlTransfer> cs_controls;
    for (const auto& op : cs_plan) {
        if (op.kind == asicen::FrontendOpKind::Control)
            cs_controls.push_back(op.transfer);
    }
    CHECK(check(cs_controls.size() >= 4 && cs_controls[2].value == 0x8f32 &&
                    cs_controls[2].index == 0x00ff && cs_controls[3].value == 0x9032 &&
                    cs_controls[3].index == 0x00ff,
                "CS tune writes source-required ffff initial TSID"));
    CHECK(check(delays.size() == 1 && delays[0] == 10,
                "tuner messages have one source-derived 10ms delay"));
    return true;
}

bool test_satellite_if_to_rf_conversion_is_exact()
{
    std::uint32_t rf = 0U;
    CHECK(check(asicen::w3u3_satellite_if_to_rf_khz(1049480U, &rf) && rf == 11727480U,
                "BS01 IF maps to the official RF row"));
    CHECK(check(asicen::w3u3_satellite_if_to_rf_khz(1613000U, &rf) && rf == 12291000U,
                "CS02 IF maps to the official RF row"));
    rf = 0xdeadbeefU;
    CHECK(check(!asicen::w3u3_satellite_if_to_rf_khz(1049481U, &rf) && rf == 0xdeadbeefU,
                "non-row IF is rejected without output mutation"));
    CHECK(
        check(!asicen::w3u3_satellite_if_to_rf_khz(1049480U, nullptr), "null output is rejected"));
    return true;
}

bool test_tune_stops_on_every_transfer_failure_and_cancel()
{
    const auto plan = asicen::plan_w3u3_satellite_tune(11727480U);
    std::size_t control_count = 0;
    for (const auto& op : plan) {
        if (op.kind == asicen::FrontendOpKind::Control)
            ++control_count;
    }
    for (std::size_t i = 0; i < control_count; ++i) {
        SatelliteTransport transport;
        transport.fail_at = i;
        const auto result = asicen::run_w3u3_satellite_tune(&transport, 11727480U);
        CHECK(check(result == asicen::SatelliteOperationResult::FailedTransfer,
                    "tune reports each injected transfer failure"));
        CHECK(check(transport.transfers.size() == i + 1,
                    "tune stops immediately at each failed transfer"));
    }
    for (std::size_t i = 0; i < control_count; ++i) {
        SatelliteTransport transport;
        transport.nack_at = i;
        const auto result = asicen::run_w3u3_satellite_tune(&transport, 11727480U);
        CHECK(check(result == asicen::SatelliteOperationResult::FailedTransfer,
                    "tune reports each injected status NACK"));
        CHECK(check(transport.transfers.size() == i + 1, "tune stops immediately at each NACK"));
    }
    for (std::size_t i = 0; i + 1 < control_count; ++i) {
        SatelliteTransport transport;
        transport.cancel_after = i + 1;
        const auto result = asicen::run_w3u3_satellite_tune(&transport, 11727480U);
        CHECK(check(result == asicen::SatelliteOperationResult::Cancelled,
                    "tune reports cancellation between every transfer"));
        CHECK(check(transport.transfers.size() == i + 1,
                    "tune stops after cancellation at each transfer boundary"));
    }
    SatelliteTransport invalid_rf;
    CHECK(check(asicen::run_w3u3_satellite_tune(&invalid_rf, 11727481U) ==
                    asicen::SatelliteOperationResult::InvalidArgument,
                "invalid RF rejected by execution API"));
    CHECK(check(invalid_rf.transfers.empty(), "invalid RF performs no I/O"));
    SatelliteTransport cancelled;
    cancelled.cancel = true;
    CHECK(check(asicen::run_w3u3_satellite_tune(&cancelled, 11727480U) ==
                    asicen::SatelliteOperationResult::Cancelled,
                "cancel before tune prevents I/O"));
    CHECK(check(cancelled.transfers.empty(), "pre-cancelled tune has no I/O"));
    SatelliteTransport expired;
    expired.expire = true;
    CHECK(check(asicen::run_w3u3_satellite_tune(&expired, 11727480U) ==
                    asicen::SatelliteOperationResult::DeadlineExceeded,
                "expired tune prevents I/O"));
    CHECK(check(expired.transfers.empty(), "expired tune has no I/O"));
    return true;
}

bool test_lock_and_tsid_reads()
{
    SatelliteTransport locked;
    locked.lock_register = 0x00;
    const auto lock_result = asicen::read_w3u3_satellite_lock(&locked);
    CHECK(check(lock_result.result == asicen::SatelliteOperationResult::Completed &&
                    lock_result.locked,
                "lock is bit10 clear"));
    CHECK(check(locked.transfers.size() == 1 &&
                    locked.transfers[0].request == asicen::Request::I2cRead &&
                    locked.transfers[0].value == 0xc332 && locked.transfers[0].index == 0x0001 &&
                    locked.transfers[0].length == 2,
                "lock reads register c3 in mode1 with status byte"));

    SatelliteTransport unlocked;
    unlocked.lock_register = 0x10;
    const auto unlocked_result = asicen::read_w3u3_satellite_lock(&unlocked);
    CHECK(check(unlocked_result.result == asicen::SatelliteOperationResult::Completed &&
                    !unlocked_result.locked,
                "lock bit set means unlocked"));
    SatelliteTransport lock_nack;
    lock_nack.nack_at = 0;
    const auto bad_lock = asicen::read_w3u3_satellite_lock(&lock_nack);
    CHECK(check(bad_lock.result == asicen::SatelliteOperationResult::FailedTransfer &&
                    !bad_lock.locked,
                "NACK cannot be interpreted as lock"));
    SatelliteTransport lock_short;
    lock_short.short_at = 0;
    CHECK(check(asicen::read_w3u3_satellite_lock(&lock_short).result ==
                    asicen::SatelliteOperationResult::ShortTransfer,
                "short lock read fails"));

    SatelliteTransport poll_locked;
    poll_locked.lock_reads_before = 2;
    poll_locked.lock_register = 0;
    const auto polled_lock = asicen::poll_w3u3_satellite_lock(&poll_locked, 5, 100);
    CHECK(check(polled_lock.result == asicen::SatelliteOperationResult::Completed &&
                    polled_lock.locked,
                "lock poll waits until checked lock bit clears"));
    CHECK(check(poll_locked.transfers.size() == 3 && poll_locked.delays.size() == 2 &&
                    poll_locked.delays[0] == 100 && poll_locked.delays[1] == 100,
                "lock poll uses bounded 100ms intervals"));

    SatelliteTransport poll_unlocked;
    const auto no_lock = asicen::poll_w3u3_satellite_lock(&poll_unlocked, 3, 100);
    CHECK(check(no_lock.result == asicen::SatelliteOperationResult::DeadlineExceeded &&
                    !no_lock.locked,
                "bounded lock poll times out without lock"));
    CHECK(check(poll_unlocked.transfers.size() == 3 && poll_unlocked.delays.size() == 2,
                "lock timeout stops after configured attempts"));
    SatelliteTransport poll_cancelled;
    poll_cancelled.cancel_after = 1;
    CHECK(check(asicen::poll_w3u3_satellite_lock(&poll_cancelled, 5, 100).result ==
                    asicen::SatelliteOperationResult::Cancelled,
                "lock poll cancellation stops before next read"));
    CHECK(check(poll_cancelled.transfers.size() == 1,
                "cancelled lock poll performs no subsequent read"));

    SatelliteTransport list_transport;
    const auto list = asicen::read_w3u3_satellite_tsids(&list_transport);
    CHECK(check(list.result == asicen::SatelliteOperationResult::Completed,
                "TSID list read succeeds"));
    CHECK(check(list.tsids == list_transport.tsids, "TSID list is decoded big endian"));
    CHECK(check(
        list_transport.transfers.size() == 1 && list_transport.transfers[0].value == 0xce32 &&
            list_transport.transfers[0].index == 0x0001 && list_transport.transfers[0].length == 17,
        "TSID list reads 16 bytes from ce in mode1"));
    SatelliteTransport list_nack;
    list_nack.nack_at = 0;
    CHECK(check(asicen::read_w3u3_satellite_tsids(&list_nack).result ==
                    asicen::SatelliteOperationResult::FailedTransfer,
                "TSID list rejects NACK"));
    SatelliteTransport list_short;
    list_short.short_at = 0;
    CHECK(check(asicen::read_w3u3_satellite_tsids(&list_short).result ==
                    asicen::SatelliteOperationResult::ShortTransfer,
                "TSID list rejects short transfer"));
    return true;
}

bool test_tsid_selection_validation_and_failures()
{
    SatelliteTransport selected;
    selected.readback_tsid = 0x1234;
    const auto result = asicen::select_w3u3_satellite_tsid(&selected, 7, selected.tsids);
    CHECK(check(result.result == asicen::SatelliteOperationResult::Completed &&
                    result.selected_tsid == 0x1234,
                "valid final slot TSID selection succeeds"));
    CHECK(check(selected.transfers.size() == 3, "TSID selection writes two bytes then reads back"));
    if (selected.transfers.size() == 3) {
        CHECK(check(selected.transfers[0].request == asicen::Request::I2cWrite &&
                        selected.transfers[0].value == 0x8f32 &&
                        selected.transfers[0].index == 0x0012 && selected.transfers[0].length == 2,
                    "TSID select writes first big-endian byte at 8f mode0"));
        CHECK(check(selected.transfers[1].value == 0x9032 && selected.transfers[1].index == 0x0034,
                    "TSID select writes second big-endian byte"));
        CHECK(check(selected.transfers[2].request == asicen::Request::I2cRead &&
                        selected.transfers[2].value == 0x8f32 &&
                        selected.transfers[2].index == 0x0000 && selected.transfers[2].length == 3,
                    "TSID select verifies by mode0 readback"));
    }

    SatelliteTransport invalid_slot;
    CHECK(check(asicen::select_w3u3_satellite_tsid(&invalid_slot, 8, invalid_slot.tsids).result ==
                    asicen::SatelliteOperationResult::InvalidArgument,
                "slot outside eight entries rejected"));
    CHECK(check(invalid_slot.transfers.empty(), "invalid slot rejected before write"));
    SatelliteTransport empty_slot;
    empty_slot.tsids[2] = asicen::kW3u3SatelliteNoTsid;
    CHECK(check(asicen::select_w3u3_satellite_tsid(&empty_slot, 2, empty_slot.tsids).result ==
                    asicen::SatelliteOperationResult::InvalidArgument,
                "empty sentinel TSID rejected"));
    CHECK(check(empty_slot.transfers.empty(), "invalid TSID rejected before write"));

    SatelliteTransport mismatch;
    mismatch.readback_tsid = 0x9999;
    CHECK(check(asicen::select_w3u3_satellite_tsid(&mismatch, 7, mismatch.tsids).result ==
                    asicen::SatelliteOperationResult::VerificationFailed,
                "TSID mismatch is not reported as success"));

    for (std::size_t i = 0; i < 3; ++i) {
        SatelliteTransport failed;
        failed.fail_at = i;
        CHECK(check(asicen::select_w3u3_satellite_tsid(&failed, 7, failed.tsids).result ==
                        asicen::SatelliteOperationResult::FailedTransfer,
                    "TSID selection reports transfer failure"));
        CHECK(check(failed.transfers.size() == i + 1, "TSID selection stops at each failed step"));
    }
    for (std::size_t i = 0; i < 3; ++i) {
        SatelliteTransport cancelled;
        cancelled.cancel_after = i + 1;
        const auto cancelled_result =
            asicen::select_w3u3_satellite_tsid(&cancelled, 7, cancelled.tsids);
        if (i < 2) {
            CHECK(check(cancelled_result.result == asicen::SatelliteOperationResult::Cancelled,
                        "TSID select cancellation halts remaining writes/readback"));
            CHECK(
                check(cancelled.transfers.size() == i + 1, "TSID select stops after cancellation"));
        }
    }
    return true;
}

class TsidReadinessTransport final : public asicen::FrontendTransport {
  public:
    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override
    {
        transfers.push_back(transfer);
        for (std::uint16_t i = 0; i < transfer.length; ++i)
            data[i] = 0;
        if (transfer.request == asicen::Request::I2cRead &&
            static_cast<std::uint8_t>(transfer.value >> 8U) == 0xceU) {
            const std::size_t n = list_reads++;
            const auto& values = snapshots[std::min(n, snapshots.size() - 1U)];
            data[0] = 1;
            for (std::size_t i = 0; i < values.size(); ++i) {
                data[1U + i * 2U] = static_cast<unsigned char>(values[i] >> 8U);
                data[2U + i * 2U] = static_cast<unsigned char>(values[i]);
            }
        } else if (transfer.request == asicen::Request::I2cWrite) {
            ++selection_writes;
            data[0] = 1;
        } else {
            data[0] = 1;
        }
        return transfer.length;
    }
    void delay_ms(unsigned ms) override
    {
        delays.push_back(ms);
        if (cancel_after_delay)
            cancel = true;
        if (expire_after_delay)
            expire = true;
    }
    bool cancelled() const override
    {
        return cancel;
    }
    bool expired() const override
    {
        return expire;
    }

    std::array<std::uint16_t, asicen::kW3u3SatelliteTsidSlots> empty{};
    std::array<std::uint16_t, asicen::kW3u3SatelliteTsidSlots> valid{0U, 0xffffU, 0x4010U, 0U,
                                                                     0U, 0U,      0U,      0U};
    std::vector<std::array<std::uint16_t, asicen::kW3u3SatelliteTsidSlots>> snapshots{empty, valid};
    std::vector<asicen::ControlTransfer> transfers;
    std::vector<unsigned> delays;
    std::size_t list_reads = 0;
    std::size_t selection_writes = 0;
    bool cancel_after_delay = false;
    bool expire_after_delay = false;
    bool cancel = false;
    bool expire = false;
};

bool test_tsid_readiness_poll_policy()
{
    TsidReadinessTransport invalid_slot;
    CHECK(
        check(asicen::wait_w3u3_satellite_slot_ready(&invalid_slot, asicen::kW3u3SatelliteTsidSlots)
                          .result == asicen::SatelliteOperationResult::InvalidArgument &&
                  invalid_slot.transfers.empty(),
              "out-of-range readiness slot is rejected before I/O"));

    TsidReadinessTransport slot;
    const auto ready = asicen::wait_w3u3_satellite_slot_ready(&slot, 2, 3, 10);
    CHECK(check(ready.result == asicen::SatelliteOperationResult::Completed && ready.slot == 2 &&
                    ready.tsids[2] == 0x4010U,
                "slot readiness retries empty list then returns requested nonempty TSID"));
    CHECK(check(slot.list_reads == 2 && slot.delays == std::vector<unsigned>{10} &&
                    slot.selection_writes == 0,
                "readiness waits only after an empty read and performs no writes"));

    TsidReadinessTransport by_id;
    const auto id_ready = asicen::wait_w3u3_satellite_tsid_ready(&by_id, 0x4010U, 3, 10);
    CHECK(
        check(id_ready.result == asicen::SatelliteOperationResult::Completed && id_ready.slot == 2,
              "TSID readiness returns the slot containing the requested value"));
    CHECK(check(by_id.selection_writes == 0, "TSID readiness polling remains read-only"));

    TsidReadinessTransport timeout;
    timeout.snapshots = {timeout.empty};
    const auto timed_out = asicen::wait_w3u3_satellite_slot_ready(&timeout, 2, 3, 10);
    CHECK(check(timed_out.result == asicen::SatelliteOperationResult::DeadlineExceeded &&
                    timeout.list_reads == 3 && timeout.delays.size() == 2 &&
                    timeout.selection_writes == 0,
                "empty target times out after bounded reads without selection writes"));

    TsidReadinessTransport cancelled;
    cancelled.snapshots = {cancelled.empty};
    cancelled.cancel_after_delay = true;
    const auto cancelled_result = asicen::wait_w3u3_satellite_slot_ready(&cancelled, 2, 3, 10);
    CHECK(check(cancelled_result.result == asicen::SatelliteOperationResult::Cancelled &&
                    cancelled.list_reads == 1 && cancelled.delays.size() == 1 &&
                    cancelled.selection_writes == 0,
                "cancellation after empty read prevents further reads and writes"));

    TsidReadinessTransport expired;
    expired.snapshots = {expired.empty};
    expired.expire_after_delay = true;
    const auto expired_result = asicen::wait_w3u3_satellite_slot_ready(&expired, 2, 3, 10);
    CHECK(check(expired_result.result == asicen::SatelliteOperationResult::DeadlineExceeded &&
                    expired.list_reads == 1 && expired.delays.size() == 1 &&
                    expired.selection_writes == 0,
                "absolute deadline after poll interval prevents selection writes"));
    return true;
}

} // namespace

bool test_all()
{
    CHECK(test_rf_vectors_and_tune_plan());
    CHECK(test_satellite_if_to_rf_conversion_is_exact());
    CHECK(test_tune_stops_on_every_transfer_failure_and_cancel());
    CHECK(test_lock_and_tsid_reads());
    CHECK(test_tsid_selection_validation_and_failures());
    CHECK(test_tsid_readiness_poll_policy());
    return true;
}

int main()
{
    return test_all() ? 0 : 1;
}
