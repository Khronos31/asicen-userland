// SPDX-License-Identifier: GPL-2.0-only
// Generic developer-probe policy follows px4-userland revision
// 1a1485d0c3e972e0a47be907edb67949564aa9a7. ASICEN topology and backend
// adapters preserve this device's register plans, firmware and lane map.
#include "asicen/diagnostic_probe.h"

#include "asicen/libusb_hardware_backend.h"
#include "asicen/firmware.h"
#include "asicen/loader_firmware.h"
#include "asicen/native_thread.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <poll.h>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace asicen {
namespace {

using px4::userland::Error;
using px4::userland::ipc::System;
volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) noexcept
{
    stop_requested = 1;
}

DiagnosticProbeArguments invalid(std::string_view message) noexcept
{
    DiagnosticProbeArguments result;
    result.error = message;
    return result;
}

bool parse_u32(std::string_view value, std::uint32_t& output) noexcept
{
    if (value.empty()) return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

bool take_value(int& index, int argc, const char* const* argv,
                std::string_view& value) noexcept
{
    if (index + 1 >= argc || argv[index + 1] == nullptr) return false;
    value = std::string_view(argv[++index]);
    return !value.empty() && value.rfind("--", 0U) != 0U;
}

bool parse_base(std::string_view text, std::uint8_t& bus, std::vector<std::uint8_t>& ports)
{
    const auto separator = text.find('-');
    std::uint32_t value = 0U;
    if (separator == std::string_view::npos || !parse_u32(text.substr(0U, separator), value) ||
        value == 0U || value > 255U)
        return false;
    bus = static_cast<std::uint8_t>(value);
    text.remove_prefix(separator + 1U);
    ports.clear();
    while (!text.empty()) {
        const auto dot = text.find('.');
        if (!parse_u32(text.substr(0U, dot), value) || value == 0U || value > 255U ||
            ports.size() == 8U)
            return false;
        ports.push_back(static_cast<std::uint8_t>(value));
        if (dot == std::string_view::npos)
            return true;
        text.remove_prefix(dot + 1U);
    }
    return false;
}

int hex_nibble(char character) noexcept
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

bool parse_apdu_hex(std::string_view text, std::vector<std::uint8_t>& output) noexcept
{
    constexpr std::size_t kMaxApduLength = 65535U;
    output.clear();
    if (text.empty()) {
        return false;
    }

    std::size_t offset = 0U;
    while (offset < text.size()) {
        if (output.size() == kMaxApduLength || offset + 2U > text.size()) {
            output.clear();
            return false;
        }
        const int high = hex_nibble(text[offset]);
        const int low = hex_nibble(text[offset + 1U]);
        if (high < 0 || low < 0) {
            output.clear();
            return false;
        }
        output.push_back(static_cast<std::uint8_t>((high << 4) | low));
        offset += 2U;
        if (offset == text.size()) {
            break;
        }
        if (text[offset] != ':') {
            output.clear();
            return false;
        }
        ++offset;
        if (offset == text.size()) {
            output.clear();
            return false;
        }
    }
    return !output.empty();
}

bool parse_repeat(std::string_view text, std::uint32_t& output) noexcept
{
    if (text.empty()) {
        return false;
    }
    std::uint32_t parsed = 0U;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed < 1U || parsed > 100000U) {
        return false;
    }
    output = parsed;
    return true;
}

unsigned int baud_rate_value(px4::userland::It930xCardBaudRate baud_rate) noexcept
{
    switch (baud_rate) {
    case px4::userland::It930xCardBaudRate::baud_9600:
        return 9600U;
    case px4::userland::It930xCardBaudRate::baud_19200:
        return 19200U;
    case px4::userland::It930xCardBaudRate::baud_38400:
        return 38400U;
    }
    return 0U;
}

void print_atr(const px4::userland::CardAtr& atr) noexcept
{
    std::printf("atr=");
    for (std::size_t index = 0U; index < atr.length; ++index) {
        std::printf("%s%02x", index == 0U ? "" : ":",
                    static_cast<unsigned int>(atr.bytes[index]));
    }
    std::printf("\n");
    std::printf("baud=%u ifsc=%u edc=%s block-timeout-ms=%u\n",
                baud_rate_value(atr.baud_rate), static_cast<unsigned int>(atr.ifsc),
                atr.edc == px4::userland::CardEdc::crc ? "crc" : "lrc",
                static_cast<unsigned int>(atr.block_timeout_ms));
}

void print_response(px4::userland::ByteView response) noexcept
{
    std::printf("response=");
    for (std::size_t index = 0U; index < response.size; ++index) {
        std::printf("%s%02x", index == 0U ? "" : ":",
                    static_cast<unsigned int>(response.data[index]));
    }
    std::printf("\nresponse-length=%zu\n", response.size);
}

void usage(DiagnosticProbeRole role, const char* name, std::FILE* output)
{
    std::fprintf(output, "usage: %s ", name);
    if (role == DiagnosticProbeRole::transport)
        std::fprintf(output, "{--base BUS-PORT | --fd FD [--fd FD]} ");
    else
        std::fprintf(output, "--base BUS-PORT ");
    std::fprintf(output, "--firmware PATH ");
    if (role == DiagnosticProbeRole::card) {
        std::fprintf(output, "(--detect | --atr | --apdu HEX [--repeat 1..100000])\n");
    } else {
        std::fprintf(output, "--device 1 --receiver N --frequency-khz KHZ ");
        if (role == DiagnosticProbeRole::transport) {
            std::fprintf(output, "--seconds 1..30 --output PATH ");
        }
        std::fprintf(output, "--tune-terrestrial\n");
        if (role == DiagnosticProbeRole::transport) {
            std::fprintf(output, "satellite mode: --tune-satellite --slot 0..7 --symbol-rate 28860 "
                                 "--rolloff 4 --lnb-voltage 0\n");
        }
    }
    std::fprintf(
        output,
        "Requires an already loaded runtime device. --base is its primary USB port path.\n");
}

struct RuntimeTarget final {
    UsbLocation primary{};
    UsbLocation sibling{};
    std::vector<std::uint8_t> primary_path;
    std::vector<std::uint8_t> sibling_path;
    const DeviceProfile* profile = nullptr;
};

bool device_path(libusb_device* device, std::vector<std::uint8_t>& path)
{
    std::array<std::uint8_t, 8U> buffer{};
    const int count = libusb_get_port_numbers(device, buffer.data(), buffer.size());
    if (count <= 0)
        return false;
    path.assign(buffer.begin(), buffer.begin() + count);
    return true;
}

bool resolve_target(libusb_context* context, const DiagnosticProbeArguments& args,
                    RuntimeTarget& target)
{
    if (args.descriptor_count != 0U) {
        LibusbDevice device;
        if (device.open(context, args.descriptors[0U]) != 0)
            return false;
        libusb_device_descriptor descriptor{};
        if (device.descriptor(&descriptor) != 0)
            return false;
        target.profile = find_profile(descriptor.idVendor, descriptor.idProduct);
        if (target.profile == nullptr ||
            args.descriptor_count != target.profile->expected_runtime_functions)
            return false;
        if (!args.base.empty()) {
            std::uint8_t bus = 0U;
            std::vector<std::uint8_t> expected;
            if (!parse_base(args.base, bus, expected) ||
                libusb_get_bus_number(device.device()) != bus ||
                !device_path(device.device(), target.primary_path) ||
                expected != target.primary_path)
                return false;
        }
        return true;
    }
    std::uint8_t bus = 0U;
    if (!parse_base(args.base, bus, target.primary_path))
        return false;
    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0)
        return false;
    unsigned matches = 0U;
    for (ssize_t index = 0; index < count; ++index) {
        std::vector<std::uint8_t> path;
        if (libusb_get_bus_number(devices[index]) != bus || !device_path(devices[index], path) ||
            path != target.primary_path)
            continue;
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[index], &descriptor) != 0)
            continue;
        target.profile = find_profile(descriptor.idVendor, descriptor.idProduct);
        if (target.profile == nullptr)
            continue;
        target.primary = {bus, libusb_get_device_address(devices[index])};
        ++matches;
    }
    if (matches == 1U && target.profile->expected_runtime_functions == 2U) {
        matches = 0U;
        target.sibling_path = target.primary_path;
        if (target.sibling_path.back() == 1U) {
            target.sibling_path.back() = 2U;
            for (ssize_t index = 0; index < count; ++index) {
                std::vector<std::uint8_t> path;
                libusb_device_descriptor descriptor{};
                if (libusb_get_bus_number(devices[index]) != bus ||
                    !device_path(devices[index], path) || path != target.sibling_path ||
                    libusb_get_device_descriptor(devices[index], &descriptor) != 0 ||
                    descriptor.idVendor != target.profile->vid ||
                    descriptor.idProduct != target.profile->pid)
                    continue;
                target.sibling = {bus, libusb_get_device_address(devices[index])};
                ++matches;
            }
        }
    }
    libusb_free_device_list(devices, 1);
    return matches == 1U;
}

std::size_t write_output(int fd, const std::uint8_t* data, std::size_t size,
                  std::chrono::steady_clock::time_point deadline)
{
    const std::size_t requested = size;
    while (size != 0U) {
        if (stop_requested != 0 || std::chrono::steady_clock::now() >= deadline)
            return requested - size;
        const ssize_t count = ::write(fd, data, size);
        if (count > 0) {
            data += count;
            size -= static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{fd, POLLOUT, 0};
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            const int polled = ::poll(
                &descriptor, 1,
                static_cast<int>(std::min<long long>(250, std::max<long long>(1, remaining))));
            if (polled < 0 && errno != EINTR)
                return requested - size;
            if (polled > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                return requested - size;
            continue;
        }
        return requested - size;
    }
    return requested;
}

} // namespace

using px4::userland::ByteView;
using px4::userland::Result;

Result<void> write_ts_probe_packet(void* context, std::size_t receiver_index,
                                   ByteView packet) noexcept
{
    if (context == nullptr || packet.data == nullptr || packet.size != 188U) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    auto& sink = *static_cast<TsProbeSink*>(context);
    if (receiver_index >= sink.counters.observed_packets.size()) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (sink.counters.observed_packets[receiver_index] == std::numeric_limits<std::size_t>::max()) {
        return Result<void>::failure(Error::USB_IO);
    }
    ++sink.counters.observed_packets[receiver_index];
    if (receiver_index != sink.selected_receiver) {
        return Result<void>::success();
    }
    if (sink.write == nullptr) return Result<void>::failure(Error::INVALID_ARGUMENT);
    if (sink.counters.selected_packets == std::numeric_limits<std::size_t>::max() ||
        sink.counters.selected_bytes > std::numeric_limits<std::size_t>::max() - packet.size ||
        sink.counters.output_bytes > std::numeric_limits<std::size_t>::max() - packet.size) {
        return Result<void>::failure(Error::USB_IO);
    }

    const std::size_t written = sink.write(sink.write_context, packet.data, packet.size);
    if (written > packet.size ||
        sink.counters.output_bytes > std::numeric_limits<std::size_t>::max() - written) {
        return Result<void>::failure(Error::USB_IO);
    }
    sink.counters.output_bytes += written;
    if (written != packet.size) return Result<void>::failure(Error::USB_IO);

    ++sink.counters.selected_packets;
    sink.counters.selected_bytes += packet.size;
    return Result<void>::success();
}

TsProbeAcceptanceResult evaluate_ts_probe_acceptance(
    std::uint32_t seconds, const TsProbeSinkCounters& sink,
    const StreamCaptureFramingCounters& demux, std::size_t selected_receiver) noexcept
{
    TsProbeAcceptanceResult result;
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    const std::size_t seconds_size = static_cast<std::size_t>(seconds);
    result.required_packets = seconds_size > max / 1000U ? max : seconds_size * 1000U;
    const auto fail = [&result](TsProbeAcceptanceFailure reason) noexcept {
        if (result.failure_count < result.failures.size()) {
            result.failures[result.failure_count++] = reason;
        }
    };

    if (sink.selected_packets < result.required_packets) {
        fail(TsProbeAcceptanceFailure::insufficient_selected_packets);
    }
    if (demux.sync_loss_events != 0U) {
        fail(TsProbeAcceptanceFailure::sync_loss_events);
    }
    for (std::size_t index = 0U; index < sink.observed_packets.size(); ++index) {
        if (index != selected_receiver && sink.observed_packets[index] != 0U) {
            fail(TsProbeAcceptanceFailure::unexpected_receiver_packets);
            break;
        }
    }
    if (selected_receiver >= sink.observed_packets.size()) {
        fail(TsProbeAcceptanceFailure::unexpected_receiver_packets);
    }
    if (selected_receiver < sink.observed_packets.size() &&
        sink.observed_packets[selected_receiver] != sink.selected_packets) {
        fail(TsProbeAcceptanceFailure::selected_observed_packets_mismatch);
    }

    const bool expected_bytes_valid = sink.selected_packets <= max / 188U;
    const std::size_t expected_bytes = expected_bytes_valid ? sink.selected_packets * 188U : 0U;
    if (!expected_bytes_valid || sink.selected_bytes != expected_bytes) {
        fail(TsProbeAcceptanceFailure::selected_bytes_mismatch);
    }
    if (!expected_bytes_valid || sink.output_bytes != expected_bytes) {
        fail(TsProbeAcceptanceFailure::output_bytes_mismatch);
    }
    if (sink.output_bytes % 188U != 0U) {
        fail(TsProbeAcceptanceFailure::output_not_packet_aligned);
    }
    if (demux.buffered_bytes >= 4U * 188U) {
        fail(TsProbeAcceptanceFailure::framer_buffered_bytes);
    }
    result.accepted = result.failure_count == 0U;
    return result;
}

const char* ts_probe_acceptance_failure_string(TsProbeAcceptanceFailure failure) noexcept
{
    switch (failure) {
    case TsProbeAcceptanceFailure::insufficient_selected_packets:
        return "selected packet floor not met";
    case TsProbeAcceptanceFailure::sync_loss_events:
        return "transport sync loss observed";
    case TsProbeAcceptanceFailure::unexpected_receiver_packets:
        return "packets observed on an unselected receiver";
    case TsProbeAcceptanceFailure::selected_observed_packets_mismatch:
        return "selected observed packet count mismatch";
    case TsProbeAcceptanceFailure::selected_bytes_mismatch:
        return "selected byte count mismatch";
    case TsProbeAcceptanceFailure::output_bytes_mismatch:
        return "output byte count mismatch";
    case TsProbeAcceptanceFailure::output_not_packet_aligned:
        return "output size is not a multiple of 188";
    case TsProbeAcceptanceFailure::framer_buffered_bytes:
        return "framer retained too many buffered bytes";
    }
    return "unknown acceptance failure";
}

int run_frontend_diagnostic(px4::userland::TunerServiceBackend& hardware,
                            StreamCaptureSource& source, DiagnosticProbeRole role,
                            const DiagnosticProbeArguments& args)
{
    const System system = args.satellite ? System::ISDB_S : System::ISDB_T;
    if ((role != DiagnosticProbeRole::frontend && role != DiagnosticProbeRole::transport) ||
        !hardware.receiver_supports(args.receiver, system) ||
        (role == DiagnosticProbeRole::transport &&
         (args.output.empty() || args.seconds == 0U || args.seconds > 30U))) {
        return 2;
    }
    int primary = 0;
    const int frontend_failure = role == DiagnosticProbeRole::frontend ? 5 : 6;
    const auto failure = [](const char* stage, Error error, int status) {
        std::fprintf(stderr, "%s failed: %s\n", stage, px4::userland::error_string(error));
        return status;
    };
    Error cleanup_error = Error::OK;
    const auto remember = [&](const px4::userland::Result<void>& result) {
        if (!result && cleanup_error == Error::OK)
            cleanup_error = result.error();
    };
    int output = -1;
    if (role == DiagnosticProbeRole::transport) {
        output = ::open(args.output.c_str(),
                        O_WRONLY | O_CREAT | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600);
        struct stat information{};
        struct stat firmware_information{};
        if (output < 0 || ::fstat(output, &information) != 0 ||
            (!S_ISREG(information.st_mode) && !S_ISFIFO(information.st_mode)) ||
            (::stat(args.firmware.c_str(), &firmware_information) == 0 &&
             information.st_dev == firmware_information.st_dev &&
             information.st_ino == firmware_information.st_ino) ||
            (S_ISREG(information.st_mode) && ::ftruncate(output, 0) != 0)) {
            if (output >= 0)
                (void)::close(output);
            std::fprintf(stderr, "cannot open regular file or pipe output\n");
            return 8;
        }
    }
    bool source_prepared = false;
    TsProbeSink sink;
    sink.selected_receiver = args.receiver;
    StreamCaptureFramingCounters framing{};
    bool power_attempted = false;
    bool power_committed = false;
    do {
        if (stop_requested != 0) {
            primary = 9;
            break;
        }
        const auto opened = hardware.open_receiver(args.receiver);
        if (!opened) {
            primary = failure("frontend open", opened.error(), frontend_failure);
            break;
        }
        if (args.satellite) {
            power_attempted = true;
            const auto power = hardware.begin_tune_power(args.receiver, system, 0U);
            if (!power) {
                primary = failure("backend power", power.error(), frontend_failure);
                break;
            }
            if (hardware.selects_satellite_stream_before_tune()) {
                const auto selected =
                    hardware.select_satellite_slot(args.receiver, args.slot, 3000U);
                if (!selected) {
                    primary = failure("satellite selection", selected.error(), frontend_failure);
                    break;
                }
            }
        }
        const auto tuned =
            args.satellite ? hardware.tune_satellite(args.receiver, args.frequency_khz, 20000U)
                           : hardware.tune_terrestrial(args.receiver, args.frequency_khz, 20000U);
        if (!tuned) {
            primary = failure("tuner lock", tuned.error(), frontend_failure);
            break;
        }
        const auto lock_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool locked = false;
        while (stop_requested == 0 && std::chrono::steady_clock::now() < lock_deadline) {
            const auto status = hardware.is_locked(args.receiver, system);
            if (!status) {
                primary = failure("demod lock", status.error(), frontend_failure);
                break;
            }
            if (status.value()) {
                locked = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!locked) {
            if (primary == 0)
                primary = stop_requested != 0 ? 9 : failure("demod lock", Error::TIMEOUT, frontend_failure);
            break;
        }
        if (args.satellite && !hardware.selects_satellite_stream_before_tune()) {
            const auto selected = hardware.select_satellite_slot(args.receiver, args.slot, 3000U);
            if (!selected) {
                primary = failure("satellite selection", selected.error(), frontend_failure);
                break;
            }
        }
        if (!args.satellite && hardware.requires_terrestrial_lock_settle()) {
            for (unsigned interval = 0U; interval < 35U && stop_requested == 0; ++interval)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (stop_requested != 0) {
                primary = 9;
                break;
            }
        }
        if (args.satellite) {
            const auto committed = hardware.commit_tune_power(args.receiver);
            if (!committed) {
                primary = failure("backend power commit", committed.error(), frontend_failure);
                break;
            }
            power_committed = true;
        }
        if (role == DiagnosticProbeRole::frontend) {
            std::printf("locked=yes\n");
            break;
        }
        std::atomic<bool> cancelled{false};
        source_prepared = true;
        const auto start = source.prepare(args.receiver, system, cancelled);
        if (!start) {
            primary = failure("stream start", start.error(), 7);
            break;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(args.seconds);
        struct OutputContext final {
            int descriptor;
            std::chrono::steady_clock::time_point deadline;
        // Like the reference, finish the accepted completion's packets after
        // the capture deadline. Bound that final output work instead of using
        // blocking stdio, so an ordinary timer race is not a file failure.
        } output_context{output, deadline + std::chrono::milliseconds(250)};
        sink.write_context = &output_context;
        sink.write = [](void* context, const void* bytes, std::size_t size) noexcept {
            const auto& output = *static_cast<OutputContext*>(context);
            return write_output(output.descriptor, static_cast<const std::uint8_t*>(bytes),
                                size, output.deadline);
        };
        struct CaptureContext final {
            TsProbeSink* sink;
            StreamCaptureSource* source;
            std::atomic<bool>* cancelled;
            std::atomic<bool> finished{false};
            std::chrono::steady_clock::time_point deadline;
            bool output_failed = false;
        } capture{&sink, &source, &cancelled, false, deadline, false};
        NativeThread timer;
        if (!timer.start([](void* opaque) -> void* {
                auto& context = *static_cast<CaptureContext*>(opaque);
                while (!context.finished.load()) {
                    if (stop_requested != 0 ||
                        std::chrono::steady_clock::now() >= context.deadline) {
                        context.cancelled->store(true);
                        context.source->interrupt();
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return nullptr;
            }, &capture)) {
            primary = failure("capture timer", Error::INTERNAL, 10);
            break;
        }
        const auto captured = source.run(cancelled,
            [](void* opaque, const std::uint8_t* bytes, std::size_t size) noexcept {
                auto& context = *static_cast<CaptureContext*>(opaque);
                if ((bytes == nullptr && size != 0U) || size % 188U != 0U) {
                    context.output_failed = true;
                    return false;
                }
                for (std::size_t offset = 0U; offset < size; offset += 188U) {
                    const auto written = write_ts_probe_packet(context.sink,
                        context.sink->selected_receiver, {bytes + offset, 188U});
                    if (!written) {
                        context.output_failed = true;
                        return false;
                    }
                }
                return true;
            }, &capture);
        capture.finished.store(true);
        timer.join();
        framing = source.source_framing_counters();
        if (stop_requested != 0) {
            primary = 9;
        } else if (capture.output_failed) {
            primary = 8;
        } else if (captured != CaptureRunResult::completed &&
                   !(captured == CaptureRunResult::cancelled && cancelled.load())) {
            primary = 7;
        }
    } while (false);
    if (stop_requested != 0 && primary == 0)
        primary = 9;
    if (source_prepared)
        remember(source.stop());
    if (power_attempted && !power_committed)
        remember(hardware.rollback_tune_power(args.receiver));
    remember(hardware.close_receiver(args.receiver));
    remember(hardware.shutdown());
    if (output >= 0 && ::close(output) != 0 && primary == 0)
        primary = 8;
    if (role == DiagnosticProbeRole::transport) {
        const auto& counters = sink.counters;
        if (primary == 0) {
            const auto acceptance = evaluate_ts_probe_acceptance(
                args.seconds, counters, framing, args.receiver);
            if (!acceptance.accepted) {
                for (std::size_t index = 0U; index < acceptance.failure_count; ++index) {
                    std::fprintf(stderr, "capture rejected: %s\n",
                        ts_probe_acceptance_failure_string(acceptance.failures[index]));
                }
                primary = 7;
            }
        }
        std::printf("selected packets=%zu bytes=%zu output=%zu observed=[%zu,%zu,%zu,%zu] "
                    "framer accepted=%zu emitted=%zu discarded=%zu sync-loss=%zu buffered=%zu\n",
                    counters.selected_packets, counters.selected_bytes, counters.output_bytes,
                    counters.observed_packets[0U], counters.observed_packets[1U],
                    counters.observed_packets[2U], counters.observed_packets[3U],
                    framing.input_bytes_accepted, framing.emitted_packets,
                    framing.discarded_sync_search_bytes, framing.sync_loss_events,
                    framing.buffered_bytes);
    }
    if (cleanup_error != Error::OK) {
        std::fprintf(stderr, "cleanup failed: %s\n", px4::userland::error_string(cleanup_error));
        if (primary == 0)
            primary = 6;
    }
    return primary;
}

// Per-role parsers below are adapted from the pinned PX4 ts_probe_support.cpp,
// frontend_probe_support.cpp and tools/card_probe.cpp. Only USB topology/fd
// function count, receiver lanes and eight hardware TSID slots differ.
namespace {

DiagnosticProbeArguments parse_ts_probe_arguments(int argc,
                                          const char* const* argv) noexcept
{
    if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
        return invalid("invalid argument vector");
    }

    DiagnosticProbeArguments result;
    bool have_base = false;
    bool have_firmware = false;
    bool have_device = false;
    bool have_receiver = false;
    bool have_frequency = false;
    bool have_seconds = false;
    bool have_output = false;
    bool have_tune = false;
    bool have_slot = false;
    bool have_symbol_rate = false;
    bool have_rolloff = false;
    bool have_lnb_voltage = false;

    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) return invalid("null argument");
        const std::string_view option(argv[index]);
        if (option == "--help") {
            if (argc != 2) return invalid("--help cannot be combined with other arguments");
            result.valid = true;
            result.help = true;
            return result;
        }
        if (option == "--tune-terrestrial") {
            if (have_tune) return invalid("duplicate --tune-terrestrial");
            have_tune = true;
            result.terrestrial = true;
            continue;
        }
        if (option == "--tune-satellite") {
            if (have_tune) return invalid("tune modes are mutually exclusive");
            have_tune = true;
            result.satellite = true;
            continue;
        }

        const bool valued = option == "--base" || option == "--firmware" ||
                            option == "--fd" ||
                            option == "--device" || option == "--receiver" ||
                            option == "--frequency-khz" || option == "--seconds" ||
                            option == "--output" || option == "--slot" ||
                            option == "--symbol-rate" || option == "--rolloff" ||
                            option == "--lnb-voltage";
        if (!valued) return invalid("unknown argument");

        std::string_view value;
        if (option == "--base") {
            if (have_base) return invalid("duplicate --base");
            if (!take_value(index, argc, argv, value)) return invalid("--base requires a value");
            have_base = true;
            result.base = value;
        } else if (option == "--fd") {
            if (result.descriptor_count >= result.descriptors.size())
                return invalid("at most two --fd values are supported");
            if (!take_value(index, argc, argv, value)) return invalid("--fd requires a value");
            std::uint32_t parsed = 0U;
            if (!parse_u32(value, parsed) ||
                parsed > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
                return invalid("--fd is invalid");
            const int fd = static_cast<int>(parsed);
            for (std::size_t fd_index = 0U;
                 fd_index < result.descriptor_count; ++fd_index) {
                if (result.descriptors[fd_index] == fd)
                    return invalid("--fd values must be distinct");
            }
            result.descriptors[result.descriptor_count++] = fd;
        } else if (option == "--firmware") {
            if (have_firmware) return invalid("duplicate --firmware");
            if (!take_value(index, argc, argv, value)) return invalid("--firmware requires a value");
            have_firmware = true;
            result.firmware = value;
        } else if (option == "--device") {
            if (have_device) return invalid("duplicate --device");
            if (!take_value(index, argc, argv, value)) return invalid("--device requires a value");
            std::uint32_t parsed = 0U;
            if (!parse_u32(value, parsed) || parsed != 1U)
                return invalid("--device must be 1");
            have_device = true;
            result.device = 1U;
        } else if (option == "--receiver") {
            if (have_receiver) return invalid("duplicate --receiver");
            if (!take_value(index, argc, argv, value)) return invalid("--receiver requires a value");
            std::uint32_t parsed = 0U;
            if (!parse_u32(value, parsed) || parsed > 1U)
                return invalid("--receiver must be 0..1");
            have_receiver = true;
            result.receiver = static_cast<std::uint8_t>(parsed);
        } else if (option == "--frequency-khz") {
            if (have_frequency) return invalid("duplicate --frequency-khz");
            if (!take_value(index, argc, argv, value)) return invalid("--frequency-khz requires a value");
            std::uint32_t parsed = 0U;
            if (!parse_u32(value, parsed)) return invalid("--frequency-khz is invalid");
            have_frequency = true;
            result.frequency_khz = parsed;
        } else if (option == "--seconds") {
            if (have_seconds) return invalid("duplicate --seconds");
            if (!take_value(index, argc, argv, value)) return invalid("--seconds requires a value");
            std::uint32_t parsed = 0U;
            if (!parse_u32(value, parsed) || parsed < 1U || parsed > 30U)
                return invalid("--seconds is outside 1..30");
            have_seconds = true;
            result.seconds = parsed;
        } else if (option == "--output") {
            if (have_output) return invalid("duplicate --output");
            if (!take_value(index, argc, argv, value)) return invalid("--output requires a value");
            have_output = true;
            result.output = value;
        } else if (option == "--slot") {
            if (have_slot) return invalid("duplicate --slot");
            if (!take_value(index, argc, argv, value)) return invalid("--slot requires a value");
            std::uint32_t parsed = 0U;
            if (!parse_u32(value, parsed) || parsed >= 8U) return invalid("--slot must be 0..7");
            have_slot = true;
            result.slot = static_cast<std::uint8_t>(parsed);
        } else if (option == "--symbol-rate") {
            if (have_symbol_rate) return invalid("duplicate --symbol-rate");
            if (!take_value(index, argc, argv, value)) return invalid("--symbol-rate requires a value");
            std::uint32_t parsed = 0U;
            if (!parse_u32(value, parsed)) return invalid("--symbol-rate is invalid");
            have_symbol_rate = true;
            result.symbol_rate = parsed;
        } else if (option == "--rolloff") {
            if (have_rolloff) return invalid("duplicate --rolloff");
            if (!take_value(index, argc, argv, value)) return invalid("--rolloff requires a value");
            std::uint32_t parsed = 0U;
            if (!parse_u32(value, parsed)) return invalid("--rolloff is invalid");
            have_rolloff = true;
            result.rolloff = parsed;
        } else if (option == "--lnb-voltage") {
            if (have_lnb_voltage) return invalid("duplicate --lnb-voltage");
            if (!take_value(index, argc, argv, value)) return invalid("--lnb-voltage requires a value");
            std::uint32_t parsed = 0U;
            if (!parse_u32(value, parsed)) return invalid("--lnb-voltage is invalid");
            have_lnb_voltage = true;
            result.lnb_voltage = parsed;
        } else {
            return invalid("unknown argument");
        }
    }

    if (result.descriptor_count == 0U) {
        if (!have_base || result.base.empty()) return invalid("--base is required");
    }
    if (!have_firmware || result.firmware.empty()) return invalid("--firmware is required");
    if (!have_device || !have_receiver) return invalid("--device and --receiver are required");
    if (!have_frequency) return invalid("--frequency-khz is required");
    if (!have_seconds) return invalid("--seconds is required");
    if (!have_output || result.output.empty()) return invalid("--output is required");
    if (!have_tune) return invalid("exactly one tune mode is required");
    if (result.terrestrial) {
        if (have_slot || have_symbol_rate || have_rolloff || have_lnb_voltage)
            return invalid("satellite-only options require --tune-satellite");
        if (result.frequency_khz < 40000U || result.frequency_khz > 1002000U)
            return invalid("--frequency-khz is outside 40000..1002000");
    } else {
        if (!have_slot || !have_symbol_rate || !have_rolloff || !have_lnb_voltage)
            return invalid("satellite requires --slot --symbol-rate --rolloff --lnb-voltage");
        if (result.device != 1U || result.receiver != 0U)
            return invalid("satellite requires --device 1 and --receiver 0");
        if (result.frequency_khz < 146875U || result.frequency_khz > 2350000U)
            return invalid("--frequency-khz is outside 146875..2350000");
        if (result.symbol_rate != 28860U) return invalid("--symbol-rate must be 28860");
        if (result.rolloff != 4U) return invalid("--rolloff must be 4");
        if (result.lnb_voltage != 0U) return invalid("--lnb-voltage must be 0");
    }

    std::uint8_t bus = 0U;
    std::vector<std::uint8_t> ports;
    if (!result.base.empty() && !parse_base(result.base, bus, ports))
        return invalid("--base must be a USB BUS-PORT topology");
    result.valid = true;
    return result;
}

DiagnosticProbeArguments parse_frontend_probe_arguments(
    int argc, const char* const* argv) noexcept
{
    if (argc < 1 || argv == nullptr) return invalid("invalid argument vector");
    DiagnosticProbeArguments result;
    bool have_base = false;
    bool have_firmware = false;
    bool have_device = false;
    bool have_receiver = false;
    bool have_frequency = false;
    bool have_tune = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) return invalid("null argument");
        const std::string_view option(argv[index]);
        if (option == "--help") {
            if (argc != 2) return invalid("--help cannot be combined with other arguments");
            result.valid = true;
            result.help = true;
            return result;
        }
        if (option == "--tune-terrestrial") {
            if (have_tune) return invalid("duplicate --tune-terrestrial");
            have_tune = true;
            result.terrestrial = true;
            continue;
        }
        if (index + 1 >= argc || argv[index + 1] == nullptr)
            return invalid("option requires one value");
        const std::string_view value(argv[++index]);
        if (option == "--base") {
            if (have_base) return invalid("duplicate --base");
            have_base = true;
            result.base = value;
        } else if (option == "--firmware") {
            if (have_firmware) return invalid("duplicate --firmware");
            have_firmware = true;
            result.firmware = value;
        } else if (option == "--device") {
            if (have_device) return invalid("duplicate --device");
            have_device = true;
            std::uint32_t parsed = 0U;
            if (!parse_u32(value, parsed) || parsed != 1U)
                return invalid("--device must be 1");
            result.device = 1U;
        } else if (option == "--receiver") {
            if (have_receiver) return invalid("duplicate --receiver");
            have_receiver = true;
            std::uint32_t parsed = 0U;
            if (!parse_u32(value, parsed) || parsed > 1U)
                return invalid("--receiver must be 0..1");
            result.receiver = static_cast<std::uint8_t>(parsed);
        } else if (option == "--frequency-khz") {
            if (have_frequency) return invalid("duplicate --frequency-khz");
            have_frequency = true;
            if (!parse_u32(value, result.frequency_khz) ||
                result.frequency_khz < 40000U || result.frequency_khz > 1002000U)
                return invalid("--frequency-khz is outside 40000..1002000");
        } else {
            return invalid("unknown argument");
        }
    }
    if (!have_base || result.base.empty()) return invalid("--base is required");
    if (!have_firmware || result.firmware.empty()) return invalid("--firmware is required");
    if (!have_device || !have_receiver || !have_frequency || !have_tune)
        return invalid("all probe options are required");
    std::uint8_t bus = 0U;
    std::vector<std::uint8_t> ports;
    if (!result.base.empty() && !parse_base(result.base, bus, ports))
        return invalid("--base must be a USB BUS-PORT topology");
    result.valid = true;
    return result;
}

DiagnosticProbeArguments parse_card_probe_arguments(int argc, const char* const* argv) noexcept
{
    if (argc < 1 || argv == nullptr) {
        return invalid("invalid argument vector");
    }

    DiagnosticProbeArguments result;
    bool have_base = false;
    bool have_firmware = false;
    bool have_apdu = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            return invalid("null argument");
        }
        const std::string_view argument(argv[index]);
        if (argument == "--help") {
            if (argc != 2) {
                return invalid("--help cannot be combined with other arguments");
            }
            result.valid = true;
            result.help = true;
            return result;
        }
        if (argument == "--detect") {
            if (result.detect) {
                return invalid("duplicate --detect");
            }
            result.detect = true;
            continue;
        }
        if (argument == "--atr") {
            if (result.atr) {
                return invalid("duplicate --atr");
            }
            result.atr = true;
            continue;
        }
        if (argument == "--apdu") {
            if (have_apdu || index + 1 >= argc || argv[index + 1] == nullptr) {
                return invalid("--apdu requires exactly one value");
            }
            have_apdu = true;
            result.apdu_mode = true;
            if (!parse_apdu_hex(argv[++index], result.apdu)) {
                return invalid(
                    "--apdu must be 1..65535 colon-separated hexadecimal bytes");
            }
            continue;
        }
        if (argument == "--repeat") {
            if (result.repeat_specified || index + 1 >= argc || argv[index + 1] == nullptr) {
                return invalid("--repeat requires exactly one value");
            }
            result.repeat_specified = true;
            if (!parse_repeat(argv[++index], result.repeat)) {
                return invalid("--repeat must be a decimal integer in 1..100000");
            }
            continue;
        }
        if (argument == "--base") {
            if (have_base || index + 1 >= argc || argv[index + 1] == nullptr) {
                return invalid("--base requires exactly one value");
            }
            have_base = true;
            result.base = argv[++index];
            continue;
        }
        if (argument == "--firmware") {
            if (have_firmware || index + 1 >= argc || argv[index + 1] == nullptr) {
                return invalid("--firmware requires exactly one value");
            }
            have_firmware = true;
            result.firmware = argv[++index];
            continue;
        }
        return invalid("unknown argument");
    }

    if (!have_base) {
        return invalid("--base is required");
    }
    std::uint8_t bus = 0U;
    std::vector<std::uint8_t> ports;
    if (result.base.empty() || !parse_base(result.base, bus, ports))
        return invalid("--base must be a USB BUS-PORT topology");
    if (!have_firmware || result.firmware.empty()) {
        return invalid("--firmware is required and must not be empty");
    }
    const unsigned int mode_count = static_cast<unsigned int>(result.detect) +
                                    static_cast<unsigned int>(result.atr) +
                                    static_cast<unsigned int>(result.apdu_mode);
    if (mode_count != 1U) {
        return invalid("exactly one of --detect, --atr or --apdu is required");
    }
    if (result.repeat_specified && !result.apdu_mode) {
        return invalid("--repeat requires --apdu");
    }
    result.valid = true;
    return result;
}

} // namespace

DiagnosticProbeArguments parse_diagnostic_probe_arguments(DiagnosticProbeRole role, int argc,
                                                          const char* const* argv)
{
    switch (role) {
    case DiagnosticProbeRole::frontend:
        return parse_frontend_probe_arguments(argc, argv);
    case DiagnosticProbeRole::transport:
        return parse_ts_probe_arguments(argc, argv);
    case DiagnosticProbeRole::card:
        return parse_card_probe_arguments(argc, argv);
    }
    return invalid("invalid probe role");
}

int run_diagnostic_probe(DiagnosticProbeRole role, int argc, const char* const* argv)
{
    const int firmware_failure = role == DiagnosticProbeRole::frontend ? 4 : 3;
    const int open_failure = role == DiagnosticProbeRole::frontend ? 3 : 4;
    const int cleanup_failure = role == DiagnosticProbeRole::card ? 8 : 6;
    const auto args = parse_diagnostic_probe_arguments(role, argc, argv);
    const char* program = argc > 0 && argv != nullptr && argv[0] != nullptr ? argv[0] : "probe";
    if (!args.valid) {
        std::fprintf(stderr, "argument error: %s\n", args.error.c_str());
        if (role == DiagnosticProbeRole::card) {
            usage(role, program, stdout);
        }
        return 2;
    }
    if (args.help) {
        usage(role, program, stdout);
        return std::fflush(stdout) == 0 ? 0 : 8;
    }
    stop_requested = 0;
    if (std::signal(SIGINT, request_stop) == SIG_ERR)
        return 10;
    if (std::signal(SIGTERM, request_stop) == SIG_ERR) {
        (void)std::signal(SIGINT, SIG_DFL);
        return 10;
    }
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        (void)std::signal(SIGINT, SIG_DFL);
        (void)std::signal(SIGTERM, SIG_DFL);
        return 10;
    }
    // ASICEN has two source-verified image manifests, unlike the reference's
    // single IT930x image. Load and reject unknown images before USB access;
    // descriptor resolution below checks the chosen image against that model.
    FirmwareProvider provider(args.firmware, ModelId::W3u3);
    auto firmware = provider.load();
    if (!firmware && firmware.error() == Error::FIRMWARE_REJECTED) {
        FirmwareProvider alternate(args.firmware, ModelId::W3u3V2);
        firmware = alternate.load();
    }
    if (!firmware) {
        std::fprintf(stderr, "firmware failed: %s\n",
                     px4::userland::error_string(firmware.error()));
        return firmware_failure;
    }
    if (stop_requested != 0) {
        return 9;
    }
    asicen::LibusbContext context_owner;
    const int initialized = context_owner.initialize(args.descriptor_count != 0U);
    if (initialized != 0) {
        std::fprintf(stderr, "runtime open failed: %s\n",
                     px4::userland::error_string(map_libusb_acquisition_error(initialized)));
        return open_failure;
    }
    libusb_context* context = context_owner.get();
    RuntimeTarget target;
    if (!resolve_target(context, args, target)) {
        std::fprintf(stderr, "runtime target not found or topology/fd count is ambiguous; load "
                             "firmware separately for a cold device\n");

        return open_failure;
    }
    const auto verified = validate_loader_firmware(target.profile->model_id,
        firmware.value().data(), firmware.value().size());
    if (verified != LoaderFirmwareRead::Ok) {

        std::fprintf(stderr, "firmware rejected for selected model\n");
        return firmware_failure;
    }
    int result = open_failure;
    {
        std::unique_ptr<LibusbW3u3Hardware> hardware;
        if (args.descriptor_count != 0U)
            hardware = std::make_unique<LibusbW3u3Hardware>(context, args.descriptors[0U],
                                                            args.descriptors[1U], target.profile);
        else
            hardware = std::make_unique<LibusbW3u3Hardware>(
                context, target.primary, target.sibling, std::move(target.primary_path),
                std::move(target.sibling_path), target.profile);
        hardware->set_allow_lnb_power(false);
        const auto claimed = hardware->claim();
        if (!claimed) {
            std::fprintf(stderr, "claim failed: %s\n",
                         px4::userland::error_string(claimed.error()));
        } else if (role == DiagnosticProbeRole::card) {
            CardProbeRequest request;
            request.mode = args.detect ? CardProbeMode::detect
                           : args.atr  ? CardProbeMode::atr
                                       : CardProbeMode::apdu;
            request.apdu = {args.apdu.data(), args.apdu.size()};
            request.repeat = args.repeat;
            const auto summary = hardware->probe_card(request, &stop_requested);
            result = summary.error == Error::OK ? 0 : stop_requested != 0 ? 9 : summary.failure_status;
            if (summary.detected || summary.error == Error::NO_CARD ||
                (request.mode == CardProbeMode::detect && summary.error == Error::OK)) {
                std::printf("card detected=%s\n", summary.detected ? "yes" : "no");
            }
            if (summary.atr_valid) {
                print_atr(summary.atr);
            }
            if (summary.error == Error::OK && request.mode == CardProbeMode::apdu) {
                print_response({summary.response.data(), summary.response.size()});
                std::printf("transmit-count=%u\n", summary.transmit_count);
            }
            if (summary.error != Error::OK)
                std::fprintf(stderr, "card probe failed: %s\n",
                             px4::userland::error_string(summary.error));
        } else {
            struct Cancellation {
                LibusbW3u3Hardware* hardware;
                std::atomic<bool> finished{false};
            } cancellation_context{hardware.get()};
            NativeThread cancellation;
            const bool launched = cancellation.start([](void* context) -> void* {
                auto& state = *static_cast<Cancellation*>(context);
                while (!state.finished.load()) {
                    if (stop_requested != 0) {
                        state.hardware->request_stop();
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                return nullptr;
            }, &cancellation_context);
            if (!launched) {
                std::fprintf(stderr, "cancellation worker creation failed\n");
                result = 10;
            } else {
                result = run_frontend_diagnostic(*hardware, *hardware, role, args);
                cancellation_context.finished.store(true);
                cancellation.join();
            }
        }
        const auto stopped = hardware->shutdown();
        if (!stopped && result == 0)
            result = cleanup_failure;
        const auto released = hardware->release();
        if (!released && result == 0)
            result = cleanup_failure;
    }

    const bool output_ok = std::fflush(stdout) == 0;
    const bool error_ok = std::fflush(stderr) == 0;
    if ((!output_ok || !error_ok) && result == 0)
        result = 8;
    return result;
}

} // namespace asicen
