// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_stream_session.h"
#include "asicen/px4_mock_backend.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace px4::userland;
constexpr std::size_t kMaxStreamTransfer = 1048576U;

bool check(bool ok, const char* message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

template <typename T> bool check(const Result<T>& result, const char* message)
{
    return check(result.has_value(), message);
}

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!check(__VA_ARGS__)) {                                                                 \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

class FakeSource final : public asicen::StreamCaptureSource {
public:
    Result<void> prepare(std::uint8_t receiver, ipc::System system,
                         const std::atomic<bool>& cancelled) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back("prepare");
        interrupted = false;
        if (prepare_error) {
            return Result<void>::failure(Error::USB_IO);
        }
        prepared = receiver == expected_receiver && system == expected_system && !cancelled.load();
        return prepared ? Result<void>::success() : Result<void>::failure(Error::UNSUPPORTED);
    }

    asicen::CaptureRunResult run(const std::atomic<bool>& cancelled,
                                 bool (*emit)(void*, const std::uint8_t*, std::size_t),
                                 void* context) noexcept override
    {
        run_started.store(true, std::memory_order_release);
        for (const auto& chunk : chunks) {
            if (cancelled.load()) {
                break;
            }
            if (!emit(context, chunk.data(), chunk.size())) {
                break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++emitted;
            }
            condition.notify_all();
        }
        if (result == asicen::CaptureRunResult::fatal_drain) {
            return result;
        }
        std::unique_lock<std::mutex> lock(mutex);
        run_started.store(true, std::memory_order_release);
        condition.notify_all();
        condition.wait(lock, [&] { return cancelled.load() || interrupted; });
        if (hold_run_after_interrupt) {
            condition.wait(lock, [&] { return release_held_run; });
        }
        return result;
    }

    asicen::CaptureRunResult run_for_receiver(
        std::uint8_t receiver, const std::atomic<bool>& cancelled,
        bool (*emit)(void*, const std::uint8_t*, std::size_t), void* context) noexcept override
    {
        run_receiver = receiver;
        return run(cancelled, emit, context);
    }

    void interrupt() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            interrupted = true;
            interrupt_seen.store(true, std::memory_order_release);
            events.push_back("interrupt");
        }
        condition.notify_all();
    }

    void interrupt_receiver(std::uint8_t receiver) noexcept override
    {
        interrupt_receiver_id = receiver;
        interrupt();
    }

    Result<void> stop() noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back("stop");
        ++stop_calls;
        return stop_error ? Result<void>::failure(Error::USB_IO) : Result<void>::success();
    }

    Result<void> stop_receiver(std::uint8_t receiver) noexcept override
    {
        stop_receiver_id = receiver;
        return stop();
    }

    TunerStreamCounters source_counters() const noexcept override { return observations; }
    TunerStreamCounters source_counters_for_receiver(std::uint8_t receiver) const noexcept override
    {
        counter_receiver = receiver;
        return observations;
    }

    bool wait_emitted(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(2), [&] { return emitted >= count; });
    }

    void release_run()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            release_held_run = true;
        }
        condition.notify_all();
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::string> events;
    std::vector<std::vector<std::uint8_t>> chunks;
    std::size_t emitted = 0;
    std::uint8_t expected_receiver = 1U;
    ipc::System expected_system = ipc::System::ISDB_T;
    int stop_calls = 0;
    std::uint8_t run_receiver = 0xffU;
    std::uint8_t interrupt_receiver_id = 0xffU;
    std::uint8_t stop_receiver_id = 0xffU;
    mutable std::uint8_t counter_receiver = 0xffU;
    bool prepared = false;
    bool interrupted = false;
    bool prepare_error = false;
    bool stop_error = false;
    bool hold_run_after_interrupt = false;
    bool release_held_run = false;
    std::atomic<bool> run_started{false};
    std::atomic<bool> interrupt_seen{false};
    asicen::CaptureRunResult result = asicen::CaptureRunResult::cancelled;
    TunerStreamCounters observations{};
};

class ShutdownTrackingFrontend final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override { return 4U; }
    bool receiver_supports(std::uint8_t receiver, ipc::System system) const noexcept override
    {
        return (receiver == 0U && system == ipc::System::ISDB_S) ||
               (receiver == 1U && system == ipc::System::ISDB_T);
    }
    Result<void> open_receiver(std::uint8_t) noexcept override { return Result<void>::success(); }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<bool> is_locked(std::uint8_t, ipc::System) noexcept override
    {
        return Result<bool>::success(true);
    }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t, std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t, std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> close_receiver(std::uint8_t) noexcept override { return Result<void>::success(); }
    Result<void> begin_tune_power(std::uint8_t receiver, ipc::System system,
                                  std::uint8_t voltage) noexcept override
    {
        ++power_calls;
        power_receiver = receiver;
        power_system = system;
        power_voltage = voltage;
        return power_error == Error::OK ? Result<void>::success()
                                        : Result<void>::failure(power_error);
    }
    Result<void> commit_tune_power(std::uint8_t receiver) noexcept override
    {
        ++commit_calls;
        power_receiver = receiver;
        return power_error == Error::OK ? Result<void>::success()
                                        : Result<void>::failure(power_error);
    }
    Result<void> rollback_tune_power(std::uint8_t receiver) noexcept override
    {
        ++rollback_calls;
        power_receiver = receiver;
        return power_error == Error::OK ? Result<void>::success()
                                        : Result<void>::failure(power_error);
    }
    Result<void> shutdown() noexcept override
    {
        ++shutdown_calls;
        return shutdown_error ? Result<void>::failure(Error::NOT_READY) : Result<void>::success();
    }
    unsigned shutdown_calls = 0;
    bool shutdown_error = false;
    unsigned power_calls = 0;
    unsigned commit_calls = 0;
    unsigned rollback_calls = 0;
    std::uint8_t power_receiver = 0xffU;
    ipc::System power_system = ipc::System::ISDB_T;
    std::uint8_t power_voltage = 0xffU;
    Error power_error = Error::OK;
};

std::vector<std::uint8_t> packet(std::uint8_t cc)
{
    std::vector<std::uint8_t> bytes(188U, 0xffU);
    bytes[0] = 0x47U;
    bytes[1] = 0x00U;
    bytes[2] = 0x20U;
    bytes[3] = static_cast<std::uint8_t>(0x10U | cc);
    return bytes;
}

TunerAttachment identity(std::uint64_t attachment_id = 8U)
{
    TunerAttachment value{};
    value.owner_client_id = 4U;
    value.lease_id = 6U;
    value.attachment_id = attachment_id;
    value.receiver = 1U;
    value.system = ipc::System::ISDB_T;
    value.nonce[0] = 0x5aU;
    return value;
}


std::vector<std::uint8_t> flatten_packets(
    const std::vector<std::vector<std::uint8_t>>& packets)
{
    std::vector<std::uint8_t> bytes;
    for (const auto& value : packets) {
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    return bytes;
}

std::vector<std::uint8_t> adaptation_packet(std::uint8_t cc, bool payload,
                                            bool discontinuity = false)
{
    auto bytes = packet(cc);
    bytes[3U] = static_cast<std::uint8_t>((payload ? 0x30U : 0x20U) | cc);
    bytes[4U] = payload ? 1U : 183U;
    bytes[5U] = discontinuity ? 0x80U : 0U;
    return bytes;
}

std::vector<std::uint8_t> pcr_packet(std::uint8_t cc)
{
    auto bytes = adaptation_packet(cc, true);
    bytes[4U] = 13U;
    bytes[5U] = 0x18U;  // PCR and OPCR; only the PCR timestamp may differ.
    for (std::size_t index = 6U; index < 18U; ++index) {
        bytes[index] = 0U;
    }
    bytes[10U] = 0x7eU;
    bytes[16U] = 0x7eU;
    return bytes;
}

bool check_quality_stream(const std::vector<std::vector<std::uint8_t>>& packets,
                          std::uint64_t continuity_errors, std::uint64_t tei_packets,
                          const char* message)
{
    asicen::MockTunerBackend frontend;
    FakeSource source;
    source.chunks = packets;
    const auto expected = flatten_packets(packets);
    asicen::HardwareStreamService service(
        frontend, source, asicen::HardwareStreamService::TestQueueCapacityBytes{expected.size()});
    CHECK(service.start_capture(1U, ipc::System::ISDB_T), "start continuity regression stream");
    const auto id = identity(32U);
    CHECK(service.attach(id), "attach continuity regression stream");
    CHECK(source.wait_emitted(packets.size()), "all continuity regression packets reach queue");
    const auto stats = service.stats(id);
    CHECK(stats && stats.value().continuity_errors == continuity_errors &&
              stats.value().tei_packets == tei_packets,
          message);
    CHECK(stats.value().packets == packets.size() && stats.value().bytes == expected.size() &&
              stats.value().queue_drops == 0U && stats.value().sync_errors == 0U &&
              stats.value().usb_errors == 0U && stats.value().empty_intervals == 0U,
          "continuity observation preserves packet, byte and independent error counters");
    const auto terminal = service.terminal(id);
    CHECK(terminal && terminal.value() == TunerStreamTerminal::none,
          "continuity observation does not change capture termination");
    std::vector<std::uint8_t> output(expected.size());
    const auto read = service.read(id, {output.data(), output.size()}, {0U});
    CHECK(read && read.value().bytes == expected.size() && !read.value().eof &&
              !read.value().timed_out && output == expected,
          "all packets, including duplicates and untrusted packets, are delivered byte-exactly");
    CHECK(service.detach(id), "detach continuity regression stream");
    CHECK(service.stop_capture(1U, ipc::System::ISDB_T), "stop continuity regression source");
    const auto final = service.final_snapshot(id);
    CHECK(final && final.value().counters.continuity_errors == continuity_errors &&
              final.value().counters.tei_packets == tei_packets &&
              final.value().counters.packets == packets.size() &&
              final.value().counters.bytes == expected.size() &&
              final.value().terminal == static_cast<std::uint8_t>(TunerStreamTerminal::stopped),
          "final snapshot retains exact continuity, TEI and delivery totals");
    CHECK(service.release_final(id), "release continuity regression identity");
    return true;
}

bool mock_stream_uses_canonical_bounds_and_attachment_identity()
{
    asicen::MockTunerStream stream;
    const auto attached = identity(80U);
    CHECK(stream.attach(attached), "mock attaches a complete identity");
    std::array<std::uint8_t, 376U> output{};
    for (const auto size : {0U, 1U, 187U, 189U, 1048477U}) {
        CHECK(stream.read(attached, {output.data(), size}, {0U}).error() == Error::INVALID_ARGUMENT,
              "mock rejects nonpacket or oversized reads before touching output");
    }
    CHECK(stream.read(attached, {nullptr, 188U}, {0U}).error() == Error::INVALID_ARGUMENT,
          "mock rejects null output");
    CHECK(stream.final_snapshot(attached).error() == Error::NOT_READY,
          "mock final snapshot is unavailable before detach");
    for (unsigned field = 0U; field < 6U; ++field) {
        auto forged = attached;
        switch (field) {
        case 0U:
            ++forged.owner_client_id;
            break;
        case 1U:
            ++forged.lease_id;
            break;
        case 2U:
            ++forged.attachment_id;
            break;
        case 3U:
            ++forged.receiver;
            break;
        case 4U:
            forged.system = ipc::System::ISDB_S;
            break;
        case 5U:
            ++forged.nonce[0];
            break;
        }
        CHECK(stream.read(forged, {output.data(), output.size()}, {0U}).error() ==
                      Error::NOT_FOUND &&
                  stream.detach(forged).error() == Error::NOT_FOUND &&
                  stream.stats(forged).error() == Error::NOT_FOUND &&
                  stream.terminal(forged).error() == Error::NOT_FOUND &&
                  stream.final_snapshot(forged).error() == Error::NOT_FOUND &&
                  stream.release_final(forged).error() == Error::NOT_FOUND,
              "every mock stream operation rejects each altered identity field");
    }
    const auto read = stream.read(attached, {output.data(), output.size()}, {0U});
    CHECK(read && read.value().bytes == output.size() && output[0] == 0x47U &&
              output[188] == 0x47U && (output[3] & 0x0fU) == 0U && (output[191] & 0x0fU) == 1U,
          "canonical mock retains exact packet size, sync and continuity coverage");
    constexpr std::size_t max_read = (kMaxStreamTransfer / 188U) * 188U;
    std::vector<std::uint8_t> maximum(max_read + 188U);
    CHECK(stream.read(attached, {maximum.data(), maximum.size()}, {0U}).error() ==
              Error::INVALID_ARGUMENT,
          "aligned read beyond the shared maximum is rejected");
    const auto maximum_read = stream.read(attached, {maximum.data(), max_read}, {0U});
    CHECK(maximum_read && maximum_read.value().bytes != 0U &&
              maximum_read.value().bytes <= max_read && maximum_read.value().bytes % 188U == 0U,
          "exact shared maximum capacity is accepted with packet-aligned delivery");
    CHECK(stream.detach(attached), "mock detaches the exact owner");
    CHECK(stream.final_snapshot(attached), "mock publishes final after detach");
    const auto eof = stream.read(attached, {output.data(), output.size()}, {0U});
    CHECK(eof && eof.value().eof, "detached mock returns terminal EOF");
    CHECK(stream.release_final(attached), "mock releases final identity");
    CHECK(stream.stats(attached).error() == Error::NOT_FOUND,
          "released mock identity cannot access retired state");
    asicen::MockTunerBackend frontend;
    CHECK(frontend.select_satellite_slot(0U, 7U, 1000U), "last ASICEN TSID slot is valid");
    CHECK(frontend.select_satellite_slot(0U, 8U, 1000U).error() == Error::INVALID_ARGUMENT,
          "mock rejects slot beyond ASICEN eight-entry table");
    return true;
}

bool start_precedes_attach_and_full_identity_is_required()
{
    asicen::MockTunerBackend frontend;
    FakeSource source;

    source.chunks = {packet(0), packet(1)};
    asicen::HardwareStreamService service(frontend, source);
    CHECK(service.start_capture(1U, ipc::System::ISDB_T), "start capture prepares source");
    CHECK(!source.run_started.load(), "packet delivery waits for an attachment");
    auto id = identity();
    CHECK(service.attach(id), "attach registers identity after capture start");
    CHECK(source.wait_emitted(2U), "attached queue receives bounded data");
    CHECK(source.run_receiver == 1U, "run is scoped to the receiver prepared by this capture");
    auto wrong_receiver = id;
    wrong_receiver.receiver = 3U;
    CHECK(service.attach(wrong_receiver).error() == Error::NOT_READY,
          "attachment cannot bind to a receiver other than the prepared capture");
    auto wrong = id;
    wrong.nonce[0] ^= 1U;
    CHECK(service.stats(wrong).error() == Error::NOT_FOUND, "nonce mismatch is rejected");
    wrong = id;
    wrong.owner_client_id++;
    CHECK(service.terminal(wrong).error() == Error::NOT_FOUND, "owner mismatch is rejected");
    std::array<std::uint8_t, 188> output{};
    const auto read = service.read(id, {output.data(), output.size()}, {20U});
    CHECK(read && read.value().bytes == output.size() && output[0] == 0x47U,
          "reader receives the pre-attach packets");
    std::array<std::uint8_t, 189> odd_output{};
    const auto odd_read = service.read(id, {odd_output.data(), odd_output.size()}, {0U});
    CHECK(!odd_read && odd_read.error() == Error::INVALID_ARGUMENT,
          "read rejects a non-packet-aligned output buffer");
    CHECK(service.read(id, {nullptr, 188U}, {0U}).error() == Error::INVALID_ARGUMENT,
          "read rejects null output");
    CHECK(service.read(id, {output.data(), 0U}, {0U}).error() == Error::INVALID_ARGUMENT,
          "read rejects empty output");
    CHECK(
        service.read(id, {output.data(), asicen::HardwareStreamService::kMaxReadBytes + 188U}, {0U})
                .error() == Error::INVALID_ARGUMENT,
        "read rejects output above the transfer limit before accessing it");
    const auto queued_stats = service.stats(id);
    CHECK(queued_stats && queued_stats.value().packets == 2U && queued_stats.value().bytes == 376U,
          "counters include queued unread packets at enqueue time");
    CHECK(service.detach(id), "detach stops and joins producer");
    CHECK(source.interrupt_receiver_id == 1U,
          "detach interrupts only the receiver owned by this capture");
    const auto drained = service.read(id, {output.data(), output.size()}, {0U});
    CHECK(drained && drained.value().bytes == 188U && output[3] == 0x11U,
          "detached attachment drains its queued tail in order");
    const auto eof = service.read(id, {output.data(), output.size()}, {0U});
    CHECK(eof && eof.value().eof && eof.value().terminal == TunerStreamTerminal::stopped,
          "drain-only attachment reports terminal EOF");
    CHECK(service.read(id, {output.data(), output.size()}, {0U}).error() == Error::NOT_FOUND,
          "terminal EOF retires the drain-only identity");
    const auto final = service.final_snapshot(id);
    CHECK(source.counter_receiver == 1U,
          "final counters are read from the receiver owned by this capture");
    CHECK(final && final.value().counters.bytes == output.size() + 188U &&
              final.value().terminal == static_cast<std::uint8_t>(TunerStreamTerminal::stopped),
          "final counters are retained for exact identity");
    CHECK(service.stop_capture(1U, ipc::System::ISDB_T), "cleanup succeeds after detach");
    CHECK(source.stop_receiver_id == 1U, "cleanup stops only the receiver owned by this capture");
    CHECK(service.release_final(id), "final state is released after protocol flush");
    CHECK(service.start_capture(1U, ipc::System::ISDB_T), "fresh generation can restart");
    CHECK(service.stop_capture(1U, ipc::System::ISDB_T), "unattached attempt can stop");
    return true;
}

bool quality_counters_follow_enqueued_packets()
{
    asicen::MockTunerBackend frontend;
    FakeSource source;

    auto first = packet(0U);
    auto tei = packet(2U);
    tei[1U] |= 0x80U;
    auto discontinuity = packet(7U);
    discontinuity[3U] = 0x37U;
    discontinuity[4U] = 1U;
    discontinuity[5U] = 0x80U;
    auto after_reset = packet(8U);
    auto null_packet = packet(9U);
    null_packet[1U] = 0x1fU;
    null_packet[2U] = 0xffU;
    source.chunks = {first, tei, discontinuity, after_reset, null_packet, null_packet};
    source.observations.sync_errors = 2U;
    source.observations.empty_intervals = 3U;
    asicen::HardwareStreamService service(
        frontend, source, asicen::HardwareStreamService::TestQueueCapacityBytes{6U * 188U});
    CHECK(service.start_capture(1U, ipc::System::ISDB_T), "start quality observation");
    const auto id = identity(31U);
    CHECK(service.attach(id), "attach quality observation");
    CHECK(source.wait_emitted(6U), "quality packets reach queue");
    // Keep this fixture byte-for-byte: TEI CC=2 is untrusted, and current
    // DI CC=7 explicitly rebases before CC=8. No trusted comparison is a
    // gap; the separate trusted-gap regression below still requires one CC error.
    const auto stats = service.stats(id);
    CHECK(stats && stats.value().packets == 6U && stats.value().bytes == 1128U &&
              stats.value().tei_packets == 1U && stats.value().continuity_errors == 0U &&
              stats.value().sync_errors == 2U && stats.value().empty_intervals == 3U,
          "quality counters include unread queue, independent TEI, DI, sync and empty intervals");
    const auto expected = flatten_packets(source.chunks);
    std::vector<std::uint8_t> output(expected.size());
    const auto read = service.read(id, {output.data(), output.size()}, {0U});
    CHECK(read && read.value().bytes == expected.size() && output == expected,
          "quality observation preserves the entire original fixture byte-for-byte");
    CHECK(service.detach(id), "detach quality observation");
    const auto snapshot = service.final_snapshot(id);
    CHECK(snapshot && snapshot.value().counters.sync_errors == 2U &&
              snapshot.value().counters.empty_intervals == 3U &&
              snapshot.value().counters.tei_packets == 1U &&
              snapshot.value().counters.continuity_errors == 0U,
          "source observations survive final snapshot");
    CHECK(service.stop_capture(1U, ipc::System::ISDB_T), "stop quality source");
    const auto stopped_snapshot = service.final_snapshot(id);
    CHECK(stopped_snapshot && stopped_snapshot.value().counters.sync_errors == 2U,
          "source observations survive hardware cleanup");
    return true;
}

bool continuity_duplicate_pairs_triples_and_conflicts()
{
    const auto first = packet(0U);
    CHECK(check_quality_stream({first, first, packet(1U)}, 0U, 0U,
                               "one exact duplicate is legal"),
          "exact duplicate pair");
    CHECK(check_quality_stream({first, first, first, packet(1U)}, 1U, 0U,
                               "the third same-CC payload is a continuity error"),
          "exact duplicate triple");
    CHECK(check_quality_stream({first, first, first, first, packet(1U)}, 2U, 0U,
                               "every payload after the duplicate pair remains an error"),
          "long duplicate run");
    auto conflict = first;
    conflict.back() ^= 1U;
    CHECK(check_quality_stream({first, conflict, conflict, packet(1U)}, 2U, 0U,
                               "conflicting same-CC payloads never gain a duplicate allowance"),
          "same-CC payload conflict");
    auto other_pid = packet(12U);
    other_pid[2U] = 0x21U;
    auto null_packet = packet(9U);
    null_packet[1U] = 0x1fU;
    null_packet[2U] = 0xffU;
    CHECK(check_quality_stream({first, other_pid, null_packet, first, packet(1U)}, 0U, 0U,
                               "trusted other PIDs and null packets preserve per-PID adjacency"),
          "multiplexed duplicate pair");
    CHECK(check_quality_stream({packet(15U), packet(0U), packet(0U), packet(1U)}, 0U, 0U,
                               "counter wrap and its one duplicate are legal"),
          "wrap duplicate pair");
    return true;
}

bool continuity_pcr_duplicate_exception_is_narrow()
{
    const auto first = pcr_packet(5U);
    auto later = first;
    later[6U] = 0x81U;
    later[7U] = 0x34U;
    later[8U] = 0x56U;
    later[9U] = 0x78U;
    later[10U] = 0xffU;
    later[11U] = 43U;  // Valid PCR extension 299.
    CHECK(check_quality_stream({first, later, packet(6U)}, 0U, 0U,
                               "a duplicate may change only the valid PCR timestamp"),
          "PCR timestamp duplicate");
    CHECK(check_quality_stream({later, first, packet(6U)}, 0U, 0U,
                               "PCR timestamp ordering is outside this CC-only observation"),
          "nonmonotonic PCR timestamp duplicate");
    auto third = later;
    third[6U] ^= 1U;
    CHECK(check_quality_stream({first, later, third, packet(6U)}, 1U, 0U,
                               "PCR timestamp variation does not permit a third duplicate"),
          "PCR timestamp triple");
    for (const std::size_t position : {1U, 3U, 4U, 5U, 12U, 16U, 17U, 18U, 80U, 187U}) {
        auto conflict = later;
        conflict[position] ^= position == 1U ? 0x40U : position == 3U ? 0x80U : 1U;
        CHECK(check_quality_stream({first, conflict, packet(6U)}, 1U, 0U,
                                   "header, OPCR, stuffing and payload changes remain conflicts"),
              "PCR exception excludes non-PCR bytes");
    }
    auto reserved = later;
    reserved[10U] &= 0xfdU;
    auto extension = later;
    extension[11U] = 44U;  // Invalid PCR extension 300.
    CHECK(check_quality_stream({first, reserved, extension, later, packet(6U)}, 1U, 0U,
                               "invalid PCR fields break duplicate adjacency without rebasing CC"),
          "invalid PCR duplicate adjacency");
    return true;
}

bool continuity_adaptation_and_discontinuity_rebase()
{
    const auto first = packet(0U);
    const auto adaptation = adaptation_packet(0U, false);
    CHECK(check_quality_stream({first, adaptation, adaptation, packet(1U)}, 0U, 0U,
                               "adaptation-only packets retain the counter without consuming it"),
          "adaptation-only continuity");
    CHECK(check_quality_stream({first, adaptation_packet(7U, false), packet(8U)}, 1U, 0U,
                               "an adaptation-only counter jump is a real continuity error"),
          "adaptation-only jump");
    CHECK(check_quality_stream({first, adaptation, first, packet(1U)}, 1U, 0U,
                               "adaptation-only packets interrupt payload duplicate adjacency"),
          "adaptation-only duplicate boundary");
    for (const bool payload : {false, true}) {
        const auto discontinuity = adaptation_packet(7U, payload, true);
        CHECK(check_quality_stream({first, discontinuity, packet(8U)}, 0U, 0U,
                                   "valid current DI rebases both adaptation-only and payload CC"),
              "DI rebases continuity");
        CHECK(check_quality_stream({first, discontinuity, packet(11U), packet(12U)}, 1U, 0U,
                                   "DI rebase gives no grace to a later trusted counter gap"),
              "trusted gap after DI");
    }
    auto zero_length = adaptation_packet(7U, true);
    zero_length[4U] = 0U;
    zero_length[5U] = 0x80U;  // Payload, not adaptation flags.
    CHECK(check_quality_stream({first, zero_length, packet(8U)}, 1U, 0U,
                               "zero-length adaptation cannot treat the first payload byte as DI"),
          "zero-length adaptation boundary");
    auto last_payload_byte = adaptation_packet(1U, true);
    last_payload_byte[4U] = 182U;
    CHECK(check_quality_stream({first, last_payload_byte, packet(2U)}, 0U, 0U,
                               "maximum payload adaptation length retains its one payload byte"),
          "maximum payload adaptation boundary");
    return true;
}

bool continuity_tei_and_invalid_packets_preserve_trusted_baselines()
{
    const auto first = packet(0U);
    auto tei = packet(2U);
    tei[1U] |= 0x80U;
    CHECK(check_quality_stream({first, tei}, 0U, 1U,
                               "TEI itself never creates a continuity error"),
          "TEI-only observation");
    CHECK(check_quality_stream({first, tei, packet(1U)}, 0U, 1U,
                               "TEI is counted independently and does not replace trusted CC"),
          "trusted baseline after TEI");
    CHECK(check_quality_stream({first, tei, packet(3U), packet(4U)}, 1U, 1U,
                               "a later trusted gap is counted once across the TEI packet"),
          "trusted-gap regression");
    CHECK(check_quality_stream({first, tei, first, packet(1U)}, 1U, 1U,
                               "same-PID TEI invalidates duplicate adjacency"),
          "TEI duplicate boundary");
    auto foreign_tei = tei;
    foreign_tei[2U] = 0x21U;
    CHECK(check_quality_stream({first, foreign_tei, first, packet(1U)}, 1U, 1U,
                               "untrusted TEI PID cannot preserve another PID's duplicate adjacency"),
          "foreign-PID TEI duplicate boundary");
    auto null_tei = tei;
    null_tei[1U] = 0x9fU;
    null_tei[2U] = 0xffU;
    CHECK(check_quality_stream({first, null_tei, first, packet(1U)}, 1U, 1U,
                               "TEI is independently counted even when the untrusted PID is null"),
          "null-PID TEI duplicate boundary");
    auto invalid = packet(7U);
    invalid[3U] = 0x07U;  // Reserved adaptation_field_control.
    CHECK(check_quality_stream({first, invalid, packet(1U)}, 0U, 0U,
                               "invalid AFC is not a CC error and cannot replace trusted CC"),
          "trusted baseline after invalid AFC");
    CHECK(check_quality_stream({first, invalid, packet(3U), packet(4U)}, 1U, 0U,
                               "a later trusted gap remains visible after an invalid packet"),
          "trusted gap after invalid AFC");
    invalid[2U] = 0x21U;
    CHECK(check_quality_stream({first, invalid, first, packet(1U)}, 1U, 0U,
                               "invalid foreign PID breaks duplicate adjacency globally"),
          "foreign-PID invalid duplicate boundary");
    auto invalid_sync = packet(7U);
    invalid_sync[0U] = 0U;
    CHECK(check_quality_stream({first, invalid_sync, first, packet(1U)}, 1U, 0U,
                               "invalid sync does not manufacture CC state or preserve adjacency"),
          "invalid-sync duplicate boundary");
    for (const std::uint8_t length : {0U, 1U, 182U, 184U, 255U}) {
        auto bad_length = adaptation_packet(7U, false, true);
        bad_length[4U] = length;
        CHECK(check_quality_stream({first, bad_length, packet(1U)}, 0U, 0U,
                                   "invalid adaptation-only length cannot apply DI or rebase CC"),
              "invalid adaptation-only length");
    }
    for (const std::uint8_t length : {183U, 184U, 255U}) {
        auto bad_length = adaptation_packet(7U, true, true);
        bad_length[4U] = length;
        CHECK(check_quality_stream({first, bad_length, first, packet(1U)}, 1U, 0U,
                                   "invalid payload adaptation length breaks adjacency despite DI"),
              "invalid payload adaptation length");
    }
    return true;
}

bool continuity_state_resets_between_capture_epochs()
{
    // Prevent accidental embedding of the approximately 1.6 MiB checker on
    // the caller's stack, including the default Windows stack.
    static_assert(sizeof(asicen::HardwareStreamService) < 64U * 1024U,
                  "the continuity checker must be stream-owned heap storage");
    asicen::MockTunerBackend frontend;
    FakeSource source;
    const auto first = packet(0U);
    auto tei = packet(7U);
    tei[1U] |= 0x80U;
    asicen::HardwareStreamService service(
        frontend, source, asicen::HardwareStreamService::TestQueueCapacityBytes{4U * 188U});
    for (unsigned epoch = 0U; epoch < 2U; ++epoch) {
        source.chunks = epoch == 0U
            ? std::vector<std::vector<std::uint8_t>>{first, first, first, tei}
            : std::vector<std::vector<std::uint8_t>>{first, first, packet(1U)};
        const auto expected = flatten_packets(source.chunks);
        const auto previous_emitted = source.emitted;  // Previous worker has been joined.
        const std::uint64_t expected_errors = epoch == 0U ? 1U : 0U;
        const auto id = identity(40U + epoch);
        CHECK(service.start_capture(1U, ipc::System::ISDB_T), "start fresh continuity epoch");
        CHECK(service.attach(id), "attach fresh continuity epoch");
        CHECK(source.wait_emitted(previous_emitted + source.chunks.size()),
              "new epoch packets reach queue");
        const auto stats = service.stats(id);
        CHECK(stats && stats.value().continuity_errors == expected_errors &&
                  stats.value().tei_packets == expected_errors &&
                  stats.value().packets == source.chunks.size() &&
                  stats.value().bytes == expected.size(),
              "a new epoch clears trusted CC, consumed duplicate state and all quality counters");
        CHECK(service.detach(id), "detach continuity epoch");
        std::vector<std::uint8_t> output(expected.size());
        const auto read = service.read(id, {output.data(), output.size()}, {0U});
        CHECK(read && read.value().bytes == expected.size() && output == expected,
              "every epoch drains byte-exactly after detach");
        std::array<std::uint8_t, 188U> tail{};
        const auto eof = service.read(id, {tail.data(), tail.size()}, {0U});
        CHECK(eof && eof.value().eof && eof.value().terminal == TunerStreamTerminal::stopped,
              "epoch drain retains the stopped terminal");
        CHECK(service.stop_capture(1U, ipc::System::ISDB_T), "stop continuity epoch source");
        const auto final = service.final_snapshot(id);
        CHECK(final && final.value().counters.continuity_errors == expected_errors &&
                  final.value().counters.tei_packets == expected_errors &&
                  final.value().counters.packets == source.chunks.size() &&
                  final.value().counters.bytes == expected.size(),
              "epoch final snapshot cannot inherit an earlier epoch's counters");
        CHECK(service.release_final(id), "release epoch before starting another stream");
    }
    return true;
}

bool thread_launch_failure_rolls_back_prepared_capture()
{
    asicen::MockTunerBackend frontend;
    FakeSource source;

    const auto fail_launch = [](pthread_t*, const pthread_attr_t*, asicen::NativeThread::Entry,
                                void*) { return 11; };
    asicen::HardwareStreamService service(
        frontend, source, asicen::HardwareStreamService::TestQueueCapacityBytes{188U}, fail_launch);
    CHECK(service.start_capture(1U, ipc::System::ISDB_T).error() == Error::INTERNAL,
          "thread exhaustion is a fallible INTERNAL result");
    CHECK(source.prepared && source.stop_calls == 1 && source.interrupt_seen.load() &&
              !source.run_started.load(),
          "failed worker launch interrupts and stops prepared capture without termination");
    return true;
}

bool production_queue_capacity_validates_packet_bounds()
{
    for (const std::size_t packets : {0U, 4095U, 262145U}) {
        asicen::MockTunerBackend frontend;
        FakeSource source;

        asicen::HardwareStreamService service(frontend, source, packets);
        CHECK(service.start_capture(1U, ipc::System::ISDB_T).error() == Error::INVALID_ARGUMENT,
              "production queue rejects out-of-range packet counts before USB preparation");
        CHECK(!source.prepared, "invalid queue performs no source setup");
    }
    asicen::MockTunerBackend frontend;
    FakeSource source;

    asicen::HardwareStreamService service(frontend, source, 4096U);
    CHECK(service.start_capture(1U, ipc::System::ISDB_T),
          "minimum production queue accepts packets");
    CHECK(service.stop_capture(1U, ipc::System::ISDB_T),
          "pre-attachment worker is cancellable and joins on stop");
    CHECK(!source.run_started.load(), "unattached capture never publishes or counts packets");
    return true;
}

bool detach_does_not_publish_stale_final_snapshot_before_worker_join()
{
    asicen::MockTunerBackend frontend;
    FakeSource source;

    source.hold_run_after_interrupt = true;
    asicen::HardwareStreamService service(frontend, source);
    CHECK(service.start_capture(1U, ipc::System::ISDB_T),
          "start capture for final-snapshot publication race");
    const auto id = identity(18U);
    CHECK(service.attach(id), "attach final-snapshot publication race");
    for (int i = 0; i < 200 && !source.run_started.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(source.run_started.load(std::memory_order_acquire), "capture worker is running");

    std::atomic<bool> detach_done{false};
    std::atomic<bool> detach_ok{false};
    std::thread detacher([&] {
        detach_ok.store(static_cast<bool>(service.detach(id)), std::memory_order_release);
        detach_done.store(true, std::memory_order_release);
    });
    struct DetachJoin {
        FakeSource& source;
        std::thread& thread;
        ~DetachJoin()
        {
            source.release_run();
            if (thread.joinable()) {
                thread.join();
            }
        }
    } detach_join{source, detacher};
    for (int i = 0; i < 200 && !source.interrupt_seen.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(source.interrupt_seen.load(std::memory_order_acquire),
          "detach reached the blocked worker join");

    std::array<std::uint8_t, 188> output{};
    const auto interim_read = service.read(id, {output.data(), output.size()}, {0U});
    CHECK(interim_read && interim_read.value().timed_out && !interim_read.value().eof &&
              interim_read.value().terminal == TunerStreamTerminal::none,
          "stream read waits for the final snapshot while detach joins");
    CHECK(service.final_snapshot(id).error() == Error::NOT_READY,
          "an unpublished default snapshot is never exposed");
    CHECK(!detach_done.load(std::memory_order_acquire), "worker join remains blocked");

    source.release_run();
    detacher.join();
    CHECK(detach_ok.load(std::memory_order_acquire), "detach completes after worker exits");
    const auto final = service.final_snapshot(id);
    CHECK(final && final.value().counters.packets == 0U &&
              final.value().terminal == static_cast<std::uint8_t>(TunerStreamTerminal::stopped),
          "only the completed detach publishes the stopped final snapshot");
    return true;
}

bool overflow_is_sticky_and_quarantines_cleanup_failure()
{
    asicen::MockTunerBackend frontend;
    FakeSource source;

    source.chunks = {packet(0), packet(1)};
    asicen::HardwareStreamService service(
        frontend, source, asicen::HardwareStreamService::TestQueueCapacityBytes{188U});
    CHECK(service.start_capture(1U, ipc::System::ISDB_T), "start for overflow case");
    auto id = identity(9U);
    CHECK(service.attach(id), "attach overflow case");
    CHECK(source.wait_emitted(1U), "first packet is queued");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto terminal = service.terminal(id);
    CHECK(terminal && terminal.value() == TunerStreamTerminal::slow_consumer,
          "queue overflow is a sticky terminal, not a silent drop");
    CHECK(service.detach(id), "overflow worker joins");
    source.stop_error = true;
    CHECK(service.stop_capture(1U, ipc::System::ISDB_T).error() == Error::USB_IO,
          "cleanup failure is returned");
    const auto final = service.final_snapshot(id);
    CHECK(final && final.value().counters.usb_errors == 1U &&
              final.value().terminal ==
                  static_cast<std::uint8_t>(TunerStreamTerminal::slow_consumer),
          "quarantined final counters retain first terminal and cleanup error");
    CHECK(service.release_final(id).error() == Error::NOT_READY,
          "quarantined snapshot cannot make the hardware reusable");
    CHECK(service.start_capture(1U, ipc::System::ISDB_T).error() == Error::USB_IO,
          "unclean session is quarantined against reuse");
    return true;
}

bool failed_prepare_attempts_cleanup_and_reports_quarantine()
{
    asicen::MockTunerBackend frontend;
    FakeSource source;

    source.prepare_error = true;
    source.stop_error = true;
    asicen::HardwareStreamService service(frontend, source);
    CHECK(service.start_capture(1U, ipc::System::ISDB_T).error() == Error::USB_IO,
          "preparation error is preserved");
    CHECK(service.start_capture(1U, ipc::System::ISDB_T).error() == Error::USB_IO,
          "failed compensating cleanup quarantines hardware");
    CHECK(source.stop_calls == 1, "partial start receives one cleanup attempt");
    return true;
}

bool fatal_callback_drain_quarantines_without_process_exit()
{
    asicen::MockTunerBackend frontend;
    FakeSource source;

    source.result = asicen::CaptureRunResult::fatal_drain;
    asicen::HardwareStreamService service(frontend, source);
    CHECK(service.start_capture(1U, ipc::System::ISDB_T), "start fatal case");
    CHECK(service.attach(identity(25U)), "attach fatal case");
    for (int i = 0; i < 200 && !source.run_started.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(service.shutdown().error() == Error::USB_IO,
          "pending callback path quarantines and reports USB_IO without terminating");
    return true;
}

bool fatal_callback_drain_allows_safe_service_destruction()
{
    const pid_t child = ::fork();
    CHECK(child >= 0, "fork fatal-drain test child");
    if (child == 0) {
        asicen::MockTunerBackend frontend;
        FakeSource source;
        source.result = asicen::CaptureRunResult::fatal_drain;

        asicen::HardwareStreamService service(frontend, source);
        if (!service.start_capture(1U, ipc::System::ISDB_T)) {
            ::_exit(101);
        }
        if (!service.attach(identity(26U))) {
            ::_exit(103);
        }
        for (int i = 0; i < 200 && !source.run_started.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (service.shutdown().error() != Error::USB_IO) {
            ::_exit(102);
        }
        ::_exit(0);
    }
    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!exited) {
        (void)::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
    }
    CHECK(exited && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "quarantined stream returns within bound without forced process exit");
    return true;
}

bool shutdown_cleans_source_and_is_sticky_against_restart()
{
    asicen::MockTunerBackend frontend;
    FakeSource source;

    asicen::HardwareStreamService service(frontend, source);
    CHECK(service.start_capture(1U, ipc::System::ISDB_T), "start shutdown case");
    CHECK(service.shutdown(), "shutdown joins worker and cleans source");
    CHECK(source.stop_calls == 1, "shutdown invokes source cleanup");
    CHECK(service.start_capture(1U, ipc::System::ISDB_T).error() == Error::NOT_READY,
          "global stop cannot be cleared by a later capture start");
    return true;
}

bool shutdown_cleanup_failure_is_reported_in_final_state()
{
    ShutdownTrackingFrontend frontend;
    FakeSource source;

    asicen::HardwareStreamService service(frontend, source);
    CHECK(service.start_capture(1U, ipc::System::ISDB_T), "start shutdown failure case");
    auto id = identity(11U);
    CHECK(service.attach(id), "attach shutdown failure case");
    CHECK(service.detach(id), "detach before cleanup failure");
    source.stop_error = true;
    frontend.shutdown_error = true;
    CHECK(service.stop_capture(1U, ipc::System::ISDB_T).error() == Error::USB_IO,
          "shutdown cleanup error is returned");
    CHECK(service.shutdown().error() == Error::USB_IO,
          "shutdown retains earlier stream cleanup failure");
    CHECK(frontend.shutdown_calls == 1U,
          "frontend shutdown still runs after quarantined stream cleanup failure");
    const auto final = service.final_snapshot(id);
    CHECK(final && final.value().counters.usb_errors == 1U &&
              final.value().terminal == static_cast<std::uint8_t>(TunerStreamTerminal::usb_error),
          "cleanup failure upgrades lifecycle stopped state into an error snapshot");
    return true;
}

bool model_capabilities_delegate_without_enabling_unsupported_paths()
{
    asicen::MockTunerBackend combined(1U);
    FakeSource source;

    asicen::HardwareStreamService service(combined, source);
    CHECK(service.receiver_count() == 1U, "single-receiver count is delegated");
    CHECK(service.receiver_supports(0U, ipc::System::ISDB_T) &&
              service.receiver_supports(0U, ipc::System::ISDB_S),
          "shared receiver inherits both supported tune systems");
    CHECK(service.open_receiver(0U), "combined receiver can open");
    CHECK(service.tune_terrestrial(0U, 557142U, 1000U),
          "terrestrial tune need not use logical receiver one");
    CHECK(service.tune_satellite(0U, 1049480U, 1000U), "same combined receiver can tune satellite");
    CHECK(service.open_receiver(1U).error() == Error::UNSUPPORTED &&
              service.start_capture(1U, ipc::System::ISDB_T).error() == Error::UNSUPPORTED,
          "out-of-model receiver rejected before source prepare");
    CHECK(source.events.empty(), "unsupported mapping never prepares the source");
    source.expected_receiver = 0U;
    CHECK(service.start_capture(0U, ipc::System::ISDB_T),
          "combined terrestrial receiver reaches source with logical receiver zero");
    CHECK(service.stop_capture(0U, ipc::System::ISDB_T), "combined terrestrial source stops");
    source.expected_system = ipc::System::ISDB_S;
    CHECK(service.start_capture(0U, ipc::System::ISDB_S),
          "same logical receiver can select satellite source after cleanup");
    CHECK(service.stop_capture(0U, ipc::System::ISDB_S), "combined satellite source stops");

    asicen::MockTunerBackend four;
    FakeSource secondary_source;
    asicen::HardwareStreamService secondary(four, secondary_source);
    CHECK(secondary.receiver_count() == 4U &&
              secondary.receiver_supports(2U, ipc::System::ISDB_S) &&
              secondary.receiver_supports(3U, ipc::System::ISDB_T),
          "wrapper accepts additional receivers only when frontend supports them");
    CHECK(secondary.tune_terrestrial(3U, 557142U, 1000U) &&
              secondary.select_satellite_slot(2U, 0U, 1000U),
          "secondary frontend operations preserve receiver routing");
    CHECK(!secondary.receiver_supports(4U, ipc::System::ISDB_S),
          "wrapper enforces delegated count boundary");
    secondary_source.expected_receiver = 3U;
    CHECK(secondary.start_capture(3U, ipc::System::ISDB_T),
          "supported secondary logical receiver reaches the selected source");
    CHECK(secondary.stop_capture(3U, ipc::System::ISDB_T), "secondary source stops");
    return true;
}

bool primary_satellite_receiver_mapping_is_explicit()
{
    ShutdownTrackingFrontend frontend;
    FakeSource source;

    asicen::HardwareStreamService service(frontend, source);
    CHECK(service.receiver_supports(0U, ipc::System::ISDB_S),
          "primary W3U3 receiver is satellite local zero");
    CHECK(service.receiver_supports(1U, ipc::System::ISDB_T),
          "second W3U3 lane retains terrestrial local one");
    CHECK(!service.receiver_supports(0U, ipc::System::ISDB_T) &&
              !service.receiver_supports(1U, ipc::System::ISDB_S) &&
              !service.receiver_supports(2U, ipc::System::ISDB_S),
          "unsupported receiver/system combinations stay unavailable");
    CHECK(service.tune_satellite(1U, 1049480U, 5000U).error() == Error::UNSUPPORTED,
          "satellite tuning cannot route onto the wrong frontend lane");
    CHECK(service.select_satellite_slot(1U, 0U, 1000U).error() == Error::UNSUPPORTED,
          "satellite TSID selection cannot route onto local one");
    return true;
}

bool lnb_requests_delegate_exact_values_and_errors()
{
    ShutdownTrackingFrontend frontend;
    FakeSource source;

    asicen::HardwareStreamService service(frontend, source);
    for (const std::uint8_t voltage : {15U, 0U}) {
        CHECK(service.begin_tune_power(0U, ipc::System::ISDB_S, voltage),
              "valid satellite LNB request delegates to frontend");
        CHECK(frontend.power_receiver == 0U && frontend.power_system == ipc::System::ISDB_S &&
                  frontend.power_voltage == voltage,
              "wrapper preserves ON/OFF and receiver/system exactly");
        CHECK(service.commit_tune_power(0U), "LNB commit delegates");
        CHECK(service.rollback_tune_power(0U), "LNB rollback delegates");
    }
    CHECK(frontend.power_calls == 2U && frontend.commit_calls == 2U &&
              frontend.rollback_calls == 2U,
          "each valid request is forwarded exactly once");
    for (const auto error : {Error::UNSUPPORTED, Error::USB_IO, Error::DISCONNECTED,
                             Error::NOT_READY, Error::TIMEOUT}) {
        frontend.power_error = error;
        CHECK(service.begin_tune_power(0U, ipc::System::ISDB_S, 15U).error() == error &&
                  service.commit_tune_power(0U).error() == error &&
                  service.rollback_tune_power(0U).error() == error,
              "frontend denial and hardware/cleanup errors cannot become success");
    }
    const auto calls = frontend.power_calls;
    CHECK(service.begin_tune_power(1U, ipc::System::ISDB_T, 15U).error() ==
                  Error::INVALID_ARGUMENT &&
              service.begin_tune_power(0U, ipc::System::ISDB_S, 13U).error() ==
                  Error::INVALID_ARGUMENT,
          "invalid voltage and terrestrial ON fail before frontend access");
    CHECK(service.begin_tune_power(2U, ipc::System::ISDB_S, 15U).error() == Error::UNSUPPORTED &&
              service.begin_tune_power(0U, ipc::System::ISDB_T, 0U).error() == Error::UNSUPPORTED,
          "wrong receiver/system cannot reach LNB controls");
    CHECK(frontend.power_calls == calls && source.events.empty(),
          "rejected power requests perform no frontend or capture operation");
    return true;
}

}  // namespace

int main()
{
    if (!mock_stream_uses_canonical_bounds_and_attachment_identity()) {
        return 1;
    }
    if (!production_queue_capacity_validates_packet_bounds()) {
        return 1;
    }
    if (!thread_launch_failure_rolls_back_prepared_capture()) {
        return 1;
    }
    if (!quality_counters_follow_enqueued_packets()) {
        return 1;
    }
    if (!continuity_duplicate_pairs_triples_and_conflicts() ||
        !continuity_pcr_duplicate_exception_is_narrow() ||
        !continuity_adaptation_and_discontinuity_rebase() ||
        !continuity_tei_and_invalid_packets_preserve_trusted_baselines() ||
        !continuity_state_resets_between_capture_epochs()) {
        return 1;
    }
    if (!start_precedes_attach_and_full_identity_is_required()) {
        return 1;
    }
    if (!detach_does_not_publish_stale_final_snapshot_before_worker_join()) {
        return 1;
    }
    if (!overflow_is_sticky_and_quarantines_cleanup_failure()) {
        return 1;
    }
    if (!failed_prepare_attempts_cleanup_and_reports_quarantine()) {
        return 1;
    }
    if (!fatal_callback_drain_quarantines_without_process_exit()) {
        return 1;
    }
    if (!fatal_callback_drain_allows_safe_service_destruction()) {
        return 1;
    }
    if (!shutdown_cleans_source_and_is_sticky_against_restart()) {
        return 1;
    }
    if (!shutdown_cleanup_failure_is_reported_in_final_state()) {
        return 1;
    }
    if (!primary_satellite_receiver_mapping_is_explicit()) {
        return 1;
    }
    if (!lnb_requests_delegate_exact_values_and_errors()) {
        return 1;
    }
    if (!model_capabilities_delegate_without_enabling_unsupported_paths()) {
        return 1;
    }
    std::cout << "hardware stream session tests passed\n";
    return 0;
}
