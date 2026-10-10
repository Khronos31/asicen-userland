// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/diagnostic_probe.h"
#include "asicen/frontend_sequence.h"
#include "asicen/libusb_hardware_backend.h"
#include "asicen/satellite_tune.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <unistd.h>
#include "../../third_party/px4-userland/userland/tests/test_temp_directory.h"

namespace {
using asicen::DiagnosticProbeRole;
using Words = std::vector<std::string>;
bool parse(DiagnosticProbeRole role, const Words& words)
{
    std::vector<const char*> pointers;
    for (const auto& word : words)
        pointers.push_back(word.c_str());
    return asicen::parse_diagnostic_probe_arguments(role, pointers.size(), pointers.data()).valid;
}

class RejectTransport final : public asicen::FrontendTransport {
  public:
    int control(const asicen::ControlTransfer&, unsigned char*) override
    {
        ++transfers;
        return -1;
    }
    void delay_ms(unsigned) override
    {
    }
    bool cancelled() const override
    {
        return false;
    }
    bool expired() const override
    {
        return false;
    }
    unsigned transfers = 0U;
};

class ZeroTsidTransport final : public asicen::FrontendTransport {
  public:
    int control(const asicen::ControlTransfer& request, unsigned char* data) override
    {
        ++transfers;
        for (std::size_t index = 0U; index < request.length; ++index)
            data[index] = 0U;
        data[0U] = 1U;
        return request.length;
    }
    void delay_ms(unsigned) override
    {
    }
    bool cancelled() const override
    {
        return false;
    }
    bool expired() const override
    {
        return false;
    }
    unsigned transfers = 0U;
};

class ProbeBackend final : public px4::userland::TunerServiceBackend {
  public:
    using Result = px4::userland::Result<void>;
    using Error = px4::userland::Error;
    std::uint8_t receiver_count() const noexcept override
    {
        return 2U;
    }
    bool receiver_supports(std::uint8_t receiver,
                           px4::userland::ipc::System) const noexcept override
    {
        return receiver < 2U;
    }
    Result step(const char* name) noexcept
    {
        calls.emplace_back(name);
        return failure == name ? Result::failure(Error::USB_IO) : Result::success();
    }
    Result open_receiver(std::uint8_t) noexcept override
    {
        return step("open");
    }
    Result tune_terrestrial(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    {
        return step("tune");
    }
    Result tune_satellite(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    {
        return step("tune");
    }
    px4::userland::Result<bool> is_locked(std::uint8_t,
                                          px4::userland::ipc::System) noexcept override
    {
        const auto result = step("lock");
        return result ? px4::userland::Result<bool>::success(true)
                      : px4::userland::Result<bool>::failure(result.error());
    }
    Result select_satellite_slot(std::uint8_t, std::uint8_t, std::uint32_t) noexcept override
    {
        return step("select");
    }
    Result select_satellite_tsid(std::uint8_t, std::uint16_t, std::uint32_t) noexcept override
    {
        return step("select");
    }
    Result close_receiver(std::uint8_t) noexcept override
    {
        return step("close");
    }
    Result begin_tune_power(std::uint8_t, px4::userland::ipc::System,
                            std::uint8_t) noexcept override
    {
        return step("power");
    }
    Result commit_tune_power(std::uint8_t) noexcept override
    {
        return step("commit");
    }
    Result rollback_tune_power(std::uint8_t) noexcept override
    {
        return step("rollback");
    }
    Result shutdown() noexcept override
    {
        return step("shutdown");
    }
    std::string failure;
    std::vector<std::string> calls;
};

class ProbeSource final : public asicen::StreamCaptureSource {
  public:
    px4::userland::Result<void> prepare(std::uint8_t, px4::userland::ipc::System,
                                        const std::atomic<bool>&) noexcept override
    {
        ++prepares;
        return fail_prepare ? px4::userland::Result<void>::failure(px4::userland::Error::USB_IO)
                            : px4::userland::Result<void>::success();
    }
    asicen::CaptureRunResult run(const std::atomic<bool>& cancelled,
                                 bool (*emit)(void*, const std::uint8_t*, std::size_t),
                                 void* context) noexcept override
    {
        if (fail_run)
            return asicen::CaptureRunResult::usb_error;
        std::vector<std::uint8_t> packets(188U * 1024U, 0U);
        for (std::size_t offset = 0U; offset < packets.size(); offset += 188U) {
            packets[offset] = 0x47U;
            packets[offset + 1U] = 0x1fU;
            packets[offset + 2U] = 0xffU;
            packets[offset + 3U] = 0x10U;
        }
        if (!emit(context, packets.data(), packets.size()))
            return asicen::CaptureRunResult::usb_error;
        while (!cancelled.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (emit_at_deadline && !emit(context, packets.data(), packets.size()))
            return asicen::CaptureRunResult::usb_error;
        return asicen::CaptureRunResult::cancelled;
    }
    void interrupt() noexcept override
    {
    }
    px4::userland::Result<void> stop() noexcept override
    {
        ++stops;
        return fail_stop ? px4::userland::Result<void>::failure(px4::userland::Error::USB_IO)
                         : px4::userland::Result<void>::success();
    }
    bool fail_prepare = false;
    bool fail_run = false;
    bool fail_stop = false;
    bool emit_at_deadline = false;
    unsigned prepares = 0U;
    unsigned stops = 0U;
};
} // namespace

int main()
{
    unsigned failures = 0U;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    };
    Words terrestrial{
        "ts-probe", "--base",     "1-2.1", "--firmware",        "firmware.bin", "--device",
        "1",        "--receiver", "1",     "--frequency-khz",   "557142",       "--seconds",
        "1",        "--output",   "-",     "--tune-terrestrial"};
    check(parse(DiagnosticProbeRole::transport, terrestrial), "terrestrial TS complete contract");
    Words satellite{"ts-probe",
                    "--base",
                    "1-2.1",
                    "--firmware",
                    "firmware.bin",
                    "--device",
                    "1",
                    "--receiver",
                    "0",
                    "--frequency-khz",
                    "1049480",
                    "--seconds",
                    "30",
                    "--output",
                    "ts.bin",
                    "--tune-satellite",
                    "--slot",
                    "0",
                    "--symbol-rate",
                    "28860",
                    "--rolloff",
                    "4",
                    "--lnb-voltage",
                    "0"};
    check(parse(DiagnosticProbeRole::transport, satellite), "satellite TS complete contract");
    for (const char* option : {"--base", "--firmware", "--device", "--receiver", "--frequency-khz",
                               "--seconds", "--output"}) {
        auto repeated = terrestrial;
        repeated.insert(repeated.end(), {option, "1"});
        check(!parse(DiagnosticProbeRole::transport, repeated), "duplicate TS option rejected");
    }
    for (const char* option : {"--slot", "--symbol-rate", "--rolloff", "--lnb-voltage"}) {
        auto repeated = satellite;
        repeated.insert(repeated.end(), {option, "0"});
        check(!parse(DiagnosticProbeRole::transport, repeated),
              "duplicate satellite option rejected");
    }
    for (const char* value : {"0", "31", "-1", "+1", "0x1", " 1", "1x", "4294967296"}) {
        auto invalid = terrestrial;
        invalid[12U] = value;
        check(!parse(DiagnosticProbeRole::transport, invalid), "strict bounded decimal duration");
    }
    auto wrong_mode = terrestrial;
    wrong_mode.emplace_back("--tune-satellite");
    check(!parse(DiagnosticProbeRole::transport, wrong_mode), "mode exclusivity");
    auto fd = terrestrial;
    fd.erase(fd.begin() + 1, fd.begin() + 3);
    fd.insert(fd.end(), {"--fd", "0"});
    check(parse(DiagnosticProbeRole::transport, fd), "single-function fd mode");
    fd.insert(fd.end(), {"--fd", "1"});
    check(parse(DiagnosticProbeRole::transport, fd), "dual-function fd mode");
    fd.insert(fd.end(), {"--fd", "2"});
    check(!parse(DiagnosticProbeRole::transport, fd), "too many fds");
    for (const auto role : {DiagnosticProbeRole::frontend, DiagnosticProbeRole::transport,
                            DiagnosticProbeRole::card}) {
        check(parse(role, {"probe", "--help"}), "help alone");
        check(!parse(role, {"probe", "--help", "--base", "1-2.1"}), "help not a validation bypass");
        check(!parse(role, {"probe", "-h"}), "help exact spelling");
    }
    Words card{"card-probe", "--base", "1-2.1", "--firmware", "firmware.bin", "--detect"};
    check(parse(DiagnosticProbeRole::card, card), "detect mode");
    card.back() = "--atr";
    check(parse(DiagnosticProbeRole::card, card), "ATR mode");
    card.back() = "--apdu";
    card.insert(card.end(), {"90:30:00:00:00", "--repeat", "100000"});
    check(parse(DiagnosticProbeRole::card, card), "APDU repeat contract");
    card.back() = "100001";
    check(!parse(DiagnosticProbeRole::card, card), "repeat upper bound");
    card.back() = "1";
    card[6U] = "90:3";
    check(!parse(DiagnosticProbeRole::card, card), "APDU strict colon hex");
    check(!parse(DiagnosticProbeRole::card,
                 {"probe", "--base", "1-2.1", "--firmware", "f", "--detect", "--repeat", "1"}),
          "repeat requires APDU");
    check(!parse(DiagnosticProbeRole::card, {"probe", "--base", "", "--firmware", "f", "--detect"}),
          "empty topology rejected before USB");
    const char* duplicate_seconds[] = {"probe", "--seconds", "1", "--seconds", "1"};
    check(asicen::parse_diagnostic_probe_arguments(DiagnosticProbeRole::transport, 5,
                                                   duplicate_seconds)
                  .error == "duplicate --seconds",
          "reference per-option TS error retained");
    const char* duplicate_repeat[] = {"probe", "--repeat", "1", "--repeat", "1"};
    check(asicen::parse_diagnostic_probe_arguments(DiagnosticProbeRole::card, 5, duplicate_repeat)
                  .error == "--repeat requires exactly one value",
          "reference per-option card error retained");
    std::array<std::uint8_t, 188U> packet{};
    asicen::TsProbeSink sink;
    sink.write = [](void*, const void*, std::size_t size) noexcept { return size; };
    for (unsigned index = 0U; index < 1000U; ++index) {
        check(static_cast<bool>(
                  asicen::write_ts_probe_packet(&sink, 1U, {packet.data(), packet.size()})),
              "selected packet write");
    }
    asicen::StreamCaptureFramingCounters framing{};
    check(asicen::evaluate_ts_probe_acceptance(1U, sink.counters, framing).accepted,
          "aligned selected packet floor accepted");
    for (unsigned boundary = 0U; boundary < 8U; ++boundary) {
        auto counters = sink.counters;
        auto invalid_framing = framing;
        switch (boundary) {
        case 0U:
            counters.selected_packets = 999U;
            break;
        case 1U:
            invalid_framing.sync_loss_events = 1U;
            break;
        case 2U:
            counters.observed_packets[0U] = 1U;
            break;
        case 3U:
            ++counters.observed_packets[1U];
            break;
        case 4U:
            ++counters.selected_bytes;
            break;
        case 5U:
            counters.output_bytes += 188U;
            break;
        case 6U:
            ++counters.output_bytes;
            break;
        case 7U:
            invalid_framing.buffered_bytes = 4U * 188U;
            break;
        }
        check(!asicen::evaluate_ts_probe_acceptance(1U, counters, invalid_framing).accepted,
              "each reference acceptance boundary rejects independently");
    }
    sink.write = [](void*, const void*, std::size_t) noexcept -> std::size_t { return 17U; };
    check(!asicen::write_ts_probe_packet(&sink, 1U, {packet.data(), packet.size()}) &&
              sink.counters.output_bytes == 188000U + 17U &&
              sink.counters.observed_packets[1U] == 1001U &&
              sink.counters.selected_packets == 1000U,
          "partial output records actual bytes and observed-only packet");
    RejectTransport invalid;
    asicen::FrontendOp operation;
    operation.transfer = asicen::make_i2c_read(0x30U, 0U, 65535U, 0U);
    check(asicen::run_frontend_plan({operation}, &invalid, nullptr) ==
                  asicen::FrontendRunResult::InvalidArgument &&
              invalid.transfers == 0U,
          "invalid builder cannot reach frontend transport");
    operation.kind = static_cast<asicen::FrontendOpKind>(255U);
    check(asicen::run_frontend_plan({operation}, &invalid, nullptr) ==
                  asicen::FrontendRunResult::InvalidArgument &&
              invalid.transfers == 0U,
          "unknown frontend operation cannot silently succeed");
    ZeroTsidTransport explicit_zero;
    const auto zero = asicen::wait_w3u3_satellite_tsid_ready(&explicit_zero, 0U, 1U, 0U);
    check(zero.result == asicen::SatelliteOperationResult::Completed && zero.slot == 0U,
          "explicit TSID zero is not a missing argument");
    ZeroTsidTransport discovery;
    check(asicen::wait_w3u3_satellite_any_ready(&discovery, 1U, 0U).result ==
              asicen::SatelliteOperationResult::DeadlineExceeded,
          "automatic discovery keeps zero empty-slot sentinel");
    asicen::LibusbW3u3Hardware unopened(nullptr, asicen::UsbLocation{}, asicen::UsbLocation{}, {},
                                        {});
    asicen::CardProbeRequest invalid_card;
    invalid_card.repeat = 0U;
    check(unopened.probe_card(invalid_card, nullptr).error ==
              px4::userland::Error::INVALID_ARGUMENT,
          "card request validated before hardware access");

    for (const char* failed : {"open", "tune", "lock", "close", "shutdown"}) {
        ProbeBackend backend;
        backend.failure = failed;
        ProbeSource source;
        asicen::DiagnosticProbeArguments args;
        args.receiver = 1U;
        args.frequency_khz = 557142U;
        const int expected = backend.failure == "close" || backend.failure == "shutdown" ? 6 : 5;
        check(asicen::run_frontend_diagnostic(backend, source, DiagnosticProbeRole::frontend,
                                              args) == expected,
              "frontend stage/cleanup failures preserve nonzero status");
        check(std::find(backend.calls.begin(), backend.calls.end(), "close") !=
                      backend.calls.end() &&
                  std::find(backend.calls.begin(), backend.calls.end(), "shutdown") !=
                      backend.calls.end(),
              "independent cleanup attempted after every failure");
    }
    std::string pattern = px4::userland::test::temporary_directory_template("asicen-probe-");
    char* temporary = ::mkdtemp(pattern.data());
    check(temporary != nullptr, "private test directory");
    if (temporary != nullptr) {
        const std::string output = std::string(temporary) + "/capture.ts";
        for (unsigned failure = 0U; failure < 5U; ++failure) {
            ProbeBackend backend;
            ProbeSource source;
            source.fail_prepare = failure == 1U;
            source.fail_run = failure == 2U;
            source.fail_stop = failure == 3U;
            source.emit_at_deadline = failure == 4U;
            asicen::DiagnosticProbeArguments args;
            args.receiver = 1U;
            args.frequency_khz = 557142U;
            args.seconds = 1U;
            args.output = output;
            const int status = asicen::run_frontend_diagnostic(
                backend, source, DiagnosticProbeRole::transport, args);
            check(failure == 0U || failure == 4U ? status == 0 : status != 0,
                  "TS source start/run/cleanup acceptance");
            check(source.stops != 0U, "TS source cleanup attempted");
            (void)::unlink(output.c_str());
        }
        (void)::rmdir(temporary);
    }
    return failures == 0U ? 0 : 1;
}
