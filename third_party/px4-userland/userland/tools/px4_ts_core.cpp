// SPDX-License-Identifier: GPL-2.0-only
// ASICEN modification, 2026-10-10: ASICEN command spelling, absent serial and receiver capacity.
// The original license notice is unchanged.
#include "px4_ts_core.h"

#include "px4/control_client.h"
#if defined(ASICEN_PRODUCT_CLI)
#include "asicen/product_profile.h"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

namespace px4::userland::cli {
namespace {

using namespace ipc;
using namespace ipc::posix;

constexpr std::uint64_t kTsProgressTimeoutMs = 5000U;

Px4TsArguments invalid(std::string_view message) noexcept
{
    Px4TsArguments result;
    result.error.assign(message.data(), message.size());
    return result;
}

bool take_value(int& index, int argc, const char* const* argv,
                std::string_view& value) noexcept
{
    if (index + 1 >= argc || argv[index + 1] == nullptr) return false;
    value = std::string_view(argv[++index]);
    return !value.empty() && value.rfind("--", 0U) != 0U;
}

template <typename T>
bool parse_unsigned(std::string_view value, T& output) noexcept
{
    if (value.empty()) return false;
    T parsed = 0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(),
                                           parsed, 10);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size())
        return false;
    output = parsed;
    return true;
}

bool parse_system(std::string_view value, System& system) noexcept
{
    if (value == "isdb-t") {
        system = System::ISDB_T;
        return true;
    }
    if (value == "isdb-s") {
        system = System::ISDB_S;
        return true;
    }
    return false;
}

// Exactly `digits` ASCII decimal digits, no sign or leading text.  Fixed digit
// counts keep T7/T027/BS1 out of the accepted grammar.
bool parse_fixed_decimal(std::string_view text, std::size_t digits,
                         unsigned int& value) noexcept
{
    if (text.size() != digits) return false;
    unsigned int parsed = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        parsed = parsed * 10U + static_cast<unsigned int>(character - '0');
    }
    value = parsed;
    return true;
}

// One or two decimal digits without a leading zero (0 and 10..99).
bool parse_short_decimal(std::string_view text, unsigned int& value) noexcept
{
    if (text.empty() || text.size() > 2U) return false;
    if (text.size() == 2U && text[0] == '0') return false;
    unsigned int parsed = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        parsed = parsed * 10U + static_cast<unsigned int>(character - '0');
    }
    value = parsed;
    return true;
}

// A 14-digit PX-Q3U4 base serial or a 15-digit PX-MLT5PE/DTV02A-5TS-P serial.
bool valid_serial(std::string_view value) noexcept
{
    if (value.size() != 14U && value.size() != 15U) return false;
    for (const char character : value) {
        if (character < '0' || character > '9') return false;
    }
    return true;
}

bool valid_instance(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 80U || value == "." || value == ".." ||
        valid_serial(value))
        return false;
    for (const char c : value) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.')
            continue;
        return false;
    }
    return true;
}

bool valid_frequency(System system, std::uint64_t frequency) noexcept
{
    if (frequency > std::numeric_limits<std::uint32_t>::max()) return false;
    return system == System::ISDB_T ? frequency >= 40000U && frequency <= 1002000U
                                    : frequency >= 146875U && frequency <= 2350000U;
}

bool valid_arguments(const Px4TsArguments& arguments) noexcept
{
    if (
#if defined(ASICEN_PRODUCT_CLI)
        (!arguments.device.empty() || !valid_instance(arguments.instance) ||
         arguments.receiver >= asicen::profile::kReceiverCount) ||
#else
        (arguments.instance.empty() ? !valid_serial(arguments.device) :
                                      (!arguments.device.empty() ||
                                       !valid_instance(arguments.instance))) ||
#endif
        arguments.frequency_khz == 0U ||
        !valid_frequency(arguments.system, arguments.frequency_khz) ||
        arguments.tune_timeout_ms < 100U || arguments.tune_timeout_ms > 30000U ||
        (arguments.duration_set && arguments.duration_seconds == 0U) ||
        (arguments.duration_set &&
         arguments.duration_seconds > std::numeric_limits<std::uint64_t>::max() / 1000U) ||
        (arguments.packet_count_set && arguments.packet_count == 0U) ||
        (arguments.duration_set && arguments.packet_count_set)) {
        return false;
    }
    if (arguments.system == System::ISDB_T) {
        return arguments.stream_id == 0xffffU && arguments.slot == 0xffffU &&
               arguments.lnb_voltage == 0U && arguments.bandwidth_hz == 6000000U;
    }
    if (arguments.bandwidth_hz != 0U || arguments.lnb_voltage > 15U ||
        (arguments.stream_id != 0xffffU && arguments.slot != 0xffffU)) {
        return false;
    }
    return (arguments.stream_id == 0xffffU && arguments.slot <= 11U) ||
           (arguments.slot == 0xffffU && arguments.stream_id != 0xffffU);
}

}  // namespace

Px4TsChannel parse_px4_ts_channel(std::string_view text) noexcept
{
    if (text.empty()) return Px4TsChannel{};
    unsigned int number = 0U;
    if (text[0] == 'T') {
        if (!parse_fixed_decimal(text.substr(1U), 2U, number) || number < 13U ||
            number > 62U)
            return Px4TsChannel{};
        Px4TsChannel result;
        result.valid = true;
        result.kind = Px4TsChannelKind::terrestrial;
        result.system = System::ISDB_T;
        result.frequency_khz = 395142U + static_cast<std::uint64_t>(number) * 6000U;
        return result;
    }
    if (text[0] >= '0' && text[0] <= '9') {
        if (!parse_fixed_decimal(text, 2U, number) || number < 13U || number > 62U)
            return Px4TsChannel{};
        Px4TsChannel result;
        result.valid = true;
        result.kind = Px4TsChannelKind::terrestrial;
        result.system = System::ISDB_T;
        result.frequency_khz = 395142U + static_cast<std::uint64_t>(number) * 6000U;
        return result;
    }
    if (text.rfind("BS", 0U) == 0U) {
        const std::string_view rest = text.substr(2U);
        const std::size_t underscore = rest.find('_');
        if (underscore == std::string_view::npos) {
            if (!parse_fixed_decimal(rest, 2U, number) || number < 1U ||
                number > 23U || (number % 2U) == 0U)
                return Px4TsChannel{};
            Px4TsChannel result;
            result.valid = true;
            result.kind = Px4TsChannelKind::satellite_transponder;
            result.system = System::ISDB_S;
            result.frequency_khz =
                1049480U + ((number - 1U) / 2U) * 38360U;
            return result;
        }
        unsigned int slot = 0U;
        if (!parse_fixed_decimal(rest.substr(0U, underscore), 2U, number) ||
            number < 1U || number > 23U || (number % 2U) == 0U ||
            !parse_short_decimal(rest.substr(underscore + 1U), slot) || slot > 11U)
            return Px4TsChannel{};
        Px4TsChannel result;
        result.valid = true;
        result.kind = Px4TsChannelKind::satellite_slot;
        result.system = System::ISDB_S;
        result.frequency_khz = 1049480U + ((number - 1U) / 2U) * 38360U;
        result.slot = static_cast<std::uint16_t>(slot);
        return result;
    }
    if (text.rfind("CS", 0U) == 0U) {
        if (!parse_short_decimal(text.substr(2U), number) || number < 2U ||
            number > 24U || (number % 2U) != 0U)
            return Px4TsChannel{};
        Px4TsChannel result;
        result.valid = true;
        result.kind = Px4TsChannelKind::satellite_cs;
        result.system = System::ISDB_S;
        result.frequency_khz = 1613000U + ((number - 2U) / 2U) * 40000U;
        result.slot = 0U;
        return result;
    }
    return Px4TsChannel{};
}

Px4TsArguments parse_px4_ts_arguments(int argc,
                                      const char* const* argv) noexcept
{
    if (argc < 1 || argv == nullptr || argv[0] == nullptr)
        return invalid("invalid argument vector");
    Px4TsArguments result;
    bool have_device = false;
    bool have_instance = false;
    bool have_runtime = false;
    bool have_receiver = false;
    bool have_system = false;
    bool have_frequency = false;
    bool have_stream_id = false;
    bool have_slot = false;
    bool have_bandwidth = false;
    bool have_lnb = false;
    bool have_timeout = false;
    bool have_channel = false;
    std::string_view channel_text;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) return invalid("null argument");
        const std::string_view option(argv[index]);
        if (option == "--help") {
            if (argc != 2) return invalid("--help cannot be combined");
            result.valid = true;
            result.help = true;
            return result;
        }
        if (option == "--group") {
            if (result.group) return invalid("duplicate --group");
            result.group = true;
            continue;
        }
        const bool takes_value = option == "--device" || option == "--instance" ||
                                 option == "--runtime-dir" ||
                                 option == "--output" || option == "--receiver" ||
                                 option == "--system" || option == "--frequency-khz" ||
                                 option == "--stream-id" || option == "--slot" ||
                                 option == "--bandwidth-hz" || option == "--lnb-voltage" ||
                                 option == "--channel" ||
                                 option == "--tune-timeout-ms" ||
                                 option == "--duration-seconds" || option == "--packet-count";
        if (!takes_value) return invalid("unknown argument");
        std::string_view value;
        if (!take_value(index, argc, argv, value)) return invalid("option requires a value");
        if (option == "--device") {
#if defined(ASICEN_PRODUCT_CLI)
            return invalid("ASICEN has no serial descriptor; use --instance");
#else
            if (have_device) return invalid("duplicate --device");
            have_device = true;
            result.device.assign(value.data(), value.size());
#endif
        } else if (option == "--instance") {
            if (have_instance) return invalid("duplicate --instance");
            have_instance = true;
            result.instance.assign(value.data(), value.size());
        } else if (option == "--runtime-dir") {
            if (have_runtime) return invalid("duplicate --runtime-dir");
            have_runtime = true;
            result.runtime_directory.assign(value.data(), value.size());
        } else if (option == "--output") {
            if (!result.output.empty()) return invalid("duplicate --output");
            result.output.assign(value.data(), value.size());
        } else if (option == "--receiver") {
            if (have_receiver || !parse_unsigned(value, result.receiver) ||
#if defined(ASICEN_PRODUCT_CLI)
                result.receiver >= asicen::profile::kReceiverCount) return invalid("receiver must be 0..3");
#else
                result.receiver >= kReceiverCount) return invalid("receiver must be 0..7");
#endif
            have_receiver = true;
        } else if (option == "--system") {
            if (have_system || !parse_system(value, result.system))
                return invalid("system must be isdb-t or isdb-s");
            have_system = true;
        } else if (option == "--frequency-khz") {
            if (have_frequency || !parse_unsigned(value, result.frequency_khz))
                return invalid("frequency-khz is invalid");
            have_frequency = true;
        } else if (option == "--channel") {
            if (have_channel) return invalid("duplicate --channel");
            have_channel = true;
            channel_text = value;
        } else if (option == "--stream-id") {
            if (have_stream_id || !parse_unsigned(value, result.stream_id) ||
                result.stream_id == 0xffffU)
                return invalid("stream-id is invalid or reserved");
            have_stream_id = true;
        } else if (option == "--slot") {
            if (have_slot || !parse_unsigned(value, result.slot) || result.slot > 11U)
                return invalid("slot must be 0..11");
            have_slot = true;
        } else if (option == "--bandwidth-hz") {
            if (have_bandwidth || !parse_unsigned(value, result.bandwidth_hz))
                return invalid("bandwidth-hz is invalid");
            have_bandwidth = true;
        } else if (option == "--lnb-voltage") {
            if (have_lnb || !parse_unsigned(value, result.lnb_voltage) ||
                (result.lnb_voltage != 0U && result.lnb_voltage != 15U))
                return invalid("lnb-voltage must be 0 or 15");
            have_lnb = true;
        } else if (option == "--tune-timeout-ms") {
            if (have_timeout || !parse_unsigned(value, result.tune_timeout_ms))
                return invalid("tune-timeout-ms is invalid");
            have_timeout = true;
        } else if (option == "--duration-seconds") {
            if (result.duration_set || !parse_unsigned(value, result.duration_seconds))
                return invalid("duration-seconds is invalid");
            result.duration_set = true;
        } else {
            if (result.packet_count_set || !parse_unsigned(value, result.packet_count))
                return invalid("packet-count is invalid");
            result.packet_count_set = true;
        }
    }
#if defined(ASICEN_PRODUCT_CLI)
    if (have_device || !have_instance)
        return invalid("ASICEN requires --instance; device serials are unavailable");
#else
    if (have_device == have_instance)
        return invalid("exactly one of --device or --instance is required");
#endif
    if (!have_receiver) return invalid("--receiver is required");
    if (have_channel) {
        if (have_system || have_frequency)
            return invalid("--channel cannot be combined with --system or --frequency-khz");
        const Px4TsChannel channel = parse_px4_ts_channel(channel_text);
        if (!channel.valid) return invalid("channel is invalid");
        result.system = channel.system;
        result.frequency_khz = channel.frequency_khz;
        if (channel.kind == Px4TsChannelKind::satellite_slot) {
            if (have_stream_id || have_slot)
                return invalid("channel already includes the slot");
            result.slot = channel.slot;
            have_slot = true;
        } else if (channel.kind == Px4TsChannelKind::satellite_cs &&
                   !have_stream_id && !have_slot) {
            result.slot = channel.slot;
            have_slot = true;
        }
    } else {
        if (!have_system) return invalid("--system is required");
        if (!have_frequency) return invalid("--frequency-khz is required");
    }
    if (result.output.empty()) result.output = "-";
    if (have_runtime && result.runtime_directory.empty())
        return invalid("--runtime-dir must not be empty");
    if (result.system == System::ISDB_T && (have_stream_id || have_slot || have_lnb ||
                                            (have_bandwidth && result.bandwidth_hz != 6000000U)))
        return invalid("satellite-only fields used with isdb-t");
    if (result.system == System::ISDB_S && !have_bandwidth) result.bandwidth_hz = 0U;
    if (result.system == System::ISDB_S && have_bandwidth && result.bandwidth_hz != 0U)
        return invalid("satellite bandwidth must be 0");
    if (have_stream_id && have_slot) return invalid("stream-id and slot are exclusive");
    if (result.system == System::ISDB_S && !have_stream_id && !have_slot)
        return invalid("isdb-s requires stream-id or slot");
    if (!valid_arguments(result)) return invalid("argument value is out of range");
    result.valid = true;
    return result;
}

void print_px4_ts_usage(void* output) noexcept
{
    FILE* file = static_cast<FILE*>(output);
    if (file == nullptr) return;
    std::fprintf(file,
#if defined(ASICEN_PRODUCT_CLI)
                 "usage: asicen-ts --instance TOKEN --receiver 0..3 "
                 "--system isdb-t|isdb-s --frequency-khz N [options]\n"
                 "       asicen-ts --instance TOKEN --receiver 0..3 "
                 "--channel CH [options]\n"
#else
                 "usage: px4-ts (--device BASE_SERIAL | --instance TOKEN) --receiver 0..7 "
                 "--system isdb-t|isdb-s --frequency-khz N [options]\n"
                 "       px4-ts (--device BASE_SERIAL | --instance TOKEN) --receiver 0..7 "
                 "--channel CH [options]\n"
#endif
                 "  --channel CH                   (T13..T62 | 13..62 | BS<nn>[_<slot>] | CS<n>)\n"
                 "  --stream-id N | --slot 0..11   (isdb-s, exactly one)\n"
                 "  --bandwidth-hz N               (isdb-t default 6000000)\n"
                 "  --lnb-voltage 0|15             (isdb-s; 15 is daemon-dependent)\n"
                 "  --tune-timeout-ms 100..30000  (default 10000)\n"
                 "  --output PATH|- --duration-seconds N | --packet-count N\n"
                 "  --runtime-dir PATH --group --help\n");
}

int px4_ts_exit_status(Error error, Px4TsFailureKind kind) noexcept
{
    if (kind == Px4TsFailureKind::ts_integrity) return 8;
    if (kind == Px4TsFailureKind::output) return 70;
    switch (error) {
    case Error::OK: return 0;
    case Error::INVALID_ARGUMENT: return 2;
    case Error::NOT_FOUND:
    case Error::NOT_READY:
    case Error::UNSUPPORTED: return 3;
    case Error::BUSY: return 4;
    case Error::TIMEOUT: return 5;
    case Error::VERSION_MISMATCH: return 6;
    case Error::USB_IO:
    case Error::DISCONNECTED: return 7;
    case Error::PROTOCOL_ERROR: return 6;
    case Error::SLOW_CONSUMER: return 8;
    case Error::NO_CARD:
    case Error::CARD_REMOVED:
    case Error::BUFFER_TOO_SMALL: return 9;
    case Error::FIRMWARE_REJECTED: return 10;
    case Error::INTERNAL: return 70;
    }
    return 70;
}

namespace {

Result<ControlResponse> request_payload(PosixControlClient& client, MessageType type,
                                         ByteView payload, Timeout timeout) noexcept
{
    return client.request(type, payload, timeout);
}

template <typename Payload>
Result<ControlResponse> request_typed(PosixControlClient& client, MessageType type,
                                      const Payload& payload, Timeout timeout) noexcept
{
    std::array<std::uint8_t, kMaxControlPayload> buffer{};
    const auto encoded = encode_payload(payload, MutableByteView{buffer.data(), buffer.size()});
    if (!encoded) return Result<ControlResponse>::failure(encoded.error());
    return request_payload(client, type, ByteView{buffer.data(), encoded.value()}, timeout);
}

bool safe_counter_bytes(const CountersPayload& counters) noexcept
{
    return counters.packets <= std::numeric_limits<std::uint64_t>::max() / 188U &&
           counters.bytes == counters.packets * 188U;
}

bool same_counters(const CountersPayload& left, const CountersPayload& right) noexcept
{
    return left.packets == right.packets && left.bytes == right.bytes &&
           left.sync_errors == right.sync_errors && left.tei_packets == right.tei_packets &&
           left.continuity_errors == right.continuity_errors &&
           left.queue_drops == right.queue_drops && left.usb_errors == right.usb_errors &&
           left.empty_intervals == right.empty_intervals;
}

bool has_ts_integrity_errors(const CountersPayload& counters) noexcept
{
    return counters.sync_errors != 0U || counters.tei_packets != 0U ||
           counters.continuity_errors != 0U || counters.queue_drops != 0U ||
           counters.usb_errors != 0U;
}

bool ts_progress_expired(std::uint64_t now, bool progress_started,
                         std::uint64_t last_progress_ms) noexcept
{
    return progress_started && now >= last_progress_ms &&
           now - last_progress_ms >= kTsProgressTimeoutMs;
}

bool failed_frame_is_ts_data(
    const StreamFramer& framer,
    const std::array<std::uint8_t, kFrameHeaderSize + kMaxTsDataPayload>& storage) noexcept
{
    if (framer.buffered_size() < kFrameHeaderSize) return false;
    const auto header = decode_frame_header(
        ByteView{storage.data(), kFrameHeaderSize});
    return header && header.value().type == MessageType::TS_DATA;
}

class StreamReader final : public FrameConsumer {
public:
    StreamReader(ConnectionStateMachine& state, Px4TsOutput& output,
                 const Px4TsArguments& arguments, std::uint64_t& packets_written,
                 std::uint64_t& expected_sequence, bool& limit_reached,
                 bool& received_end, StreamEndEventPayload& end,
                 Error& failure, Px4TsFailureKind& failure_kind,
                 bool& stop_requested_by_client, const Px4TsSignal& signal,
                 Px4TsClock& clock, bool& duration_started,
                 std::uint64_t& duration_started_ms,
                 bool& progress_started, std::uint64_t& last_progress_ms) noexcept
        : state_(state), output_(output), arguments_(arguments),
          packets_written_(packets_written), expected_sequence_(expected_sequence),
          limit_reached_(limit_reached), received_end_(received_end), end_(end),
          failure_(failure), failure_kind_(failure_kind),
          stop_requested_by_client_(stop_requested_by_client), signal_(signal),
          clock_(clock), duration_started_(duration_started),
          duration_started_ms_(duration_started_ms),
          progress_started_(progress_started), last_progress_ms_(last_progress_ms)
    {
    }

    Result<void> on_frame(const FrameView& frame) noexcept override
    {
        if (frame.header.type == MessageType::TS_DATA) {
            const auto data = decode_ts_data_event_payload(frame.payload);
            if (!data) {
                failure_ = Error::PROTOCOL_ERROR;
                failure_kind_ = Px4TsFailureKind::ts_integrity;
                return Result<void>::failure(failure_);
            }
            if (data.value().cumulative_drop_count != 0U) {
                failure_ = Error::SLOW_CONSUMER;
                failure_kind_ = Px4TsFailureKind::ts_integrity;
                return Result<void>::failure(failure_);
            }
        }
        const bool was_active = state_.phase() == ConnectionPhase::active;
        const auto accepted = state_.process_inbound(frame);
        if (!accepted) {
            failure_ = accepted.error();
            if (was_active && frame.header.type == MessageType::TS_DATA &&
                failure_ == Error::PROTOCOL_ERROR)
                failure_kind_ = Px4TsFailureKind::ts_integrity;
            return Result<void>::failure(failure_);
        }
        if (accepted.value().event == ConnectionEvent::response_accepted) {
            if (frame.header.kind == MessageKind::error_response) {
                const auto error = decode_error_response_payload(frame.payload);
                failure_ = error ? static_cast<Error>(error.value().error_code) :
                                   Error::PROTOCOL_ERROR;
                return Result<void>::failure(failure_);
            }
            if (frame.header.type == MessageType::ATTACH_STREAM) {
                progress_started_ = true;
                last_progress_ms_ = clock_.monotonic_ms();
            }
            return Result<void>::success();
        }
        if (accepted.value().event == ConnectionEvent::stream_end_accepted) {
            const auto decoded = decode_stream_end_event_payload(frame.payload);
            if (!decoded || !safe_counter_bytes(decoded.value().counters)) {
                failure_ = Error::PROTOCOL_ERROR;
                return Result<void>::failure(failure_);
            }
            end_ = decoded.value();
            received_end_ = true;
            return Result<void>::success();
        }
        const auto data = decode_ts_data_event_payload(frame.payload);
        if (!data || data.value().bytes.size == 0U ||
            (data.value().bytes.size % 188U) != 0U ||
            data.value().bytes.size > kMaxTsBytes || data.value().bytes.data == nullptr ||
            data.value().cumulative_drop_count != 0U) {
            failure_ = data && data.value().cumulative_drop_count != 0U ?
                            Error::SLOW_CONSUMER : Error::PROTOCOL_ERROR;
            failure_kind_ = Px4TsFailureKind::ts_integrity;
            return Result<void>::failure(failure_);
        }
        if (data.value().sequence != expected_sequence_) {
            failure_ = Error::PROTOCOL_ERROR;
            failure_kind_ = Px4TsFailureKind::ts_integrity;
            return Result<void>::failure(failure_);
        }
        ++expected_sequence_;
        for (std::size_t offset = 0U; offset < data.value().bytes.size; offset += 188U) {
            if (data.value().bytes.data[offset] != 0x47U) {
                failure_ = Error::PROTOCOL_ERROR;
                failure_kind_ = Px4TsFailureKind::ts_integrity;
                return Result<void>::failure(failure_);
            }
        }
        std::size_t write_size = data.value().bytes.size;
        if (arguments_.packet_count_set) {
            const std::uint64_t remaining = arguments_.packet_count - packets_written_;
            const std::uint64_t available = data.value().bytes.size / 188U;
            write_size = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, available) * 188U);
        }
        if (write_size != 0U) {
            const std::size_t packets = write_size / 188U;
            for (std::size_t packet = 0U; packet < packets; ++packet) {
                const auto written = output_.write(
                    ByteView{data.value().bytes.data + packet * 188U, 188U}, signal_);
                if (!written) {
                    if (signal_.stop_requested() && written.error() == Error::TIMEOUT) {
                        stop_requested_by_client_ = true;
                        return Result<void>::success();
                    }
                    failure_ = written.error();
                    failure_kind_ = Px4TsFailureKind::output;
                    return Result<void>::failure(failure_);
                }
                // --duration measures bytes made public to the consumer, not
                // daemon-side ATTACH/pre-roll time.  Start at the first full
                // packet successfully accepted by the output adapter.
                if (arguments_.duration_set && !duration_started_) {
                    duration_started_ms_ = clock_.monotonic_ms();
                    duration_started_ = true;
                }
            }
            packets_written_ += packets;
            progress_started_ = true;
            last_progress_ms_ = clock_.monotonic_ms();
        }
        if (arguments_.packet_count_set && packets_written_ == arguments_.packet_count)
            limit_reached_ = true;
        return Result<void>::success();
    }

private:
    ConnectionStateMachine& state_;
    Px4TsOutput& output_;
    const Px4TsArguments& arguments_;
    std::uint64_t& packets_written_;
    std::uint64_t& expected_sequence_;
    bool& limit_reached_;
    bool& received_end_;
    StreamEndEventPayload& end_;
    Error& failure_;
    Px4TsFailureKind& failure_kind_;
    bool& stop_requested_by_client_;
    const Px4TsSignal& signal_;
    Px4TsClock& clock_;
    bool& duration_started_;
    std::uint64_t& duration_started_ms_;
    bool& progress_started_;
    std::uint64_t& last_progress_ms_;
};

}  // namespace

Result<Px4TsRunResult> Px4TsRunner::run(
    const Px4TsArguments& arguments, Px4TsOutput& output, Px4TsClock& clock,
    const Px4TsSignal& signal, Px4TsFailureKind* failure_kind,
    Px4TsRunDiagnostics* diagnostics) noexcept
{
    if (failure_kind != nullptr) *failure_kind = Px4TsFailureKind::ipc;
    if (diagnostics != nullptr) *diagnostics = Px4TsRunDiagnostics{};
    Px4TsFailureKind local_failure_kind = Px4TsFailureKind::ipc;
    Px4TsFailureKind& result_failure_kind = failure_kind != nullptr ?
                                                 *failure_kind : local_failure_kind;
    if (!arguments.valid || !valid_arguments(arguments)) {
        return Result<Px4TsRunResult>::failure(Error::INVALID_ARGUMENT);
    }
    const char* runtime_directory = arguments.runtime_directory.empty() ?
                                        nullptr : arguments.runtime_directory.c_str();
    const EndpointAccess access = arguments.group ? EndpointAccess::shared_group :
                                                    EndpointAccess::private_user;
    const char* instance = arguments.instance.empty() ? arguments.device.c_str() :
                                                       arguments.instance.c_str();
    const EndpointConfig control_endpoint{runtime_directory, instance,
                                          kControlEndpointName, access};
    const EndpointConfig stream_endpoint{runtime_directory, instance,
                                         kStreamEndpointName, access};
    auto control = PosixControlClient::connect(
        control_endpoint, kCapabilityStreamStats | kCapabilityEvents, Timeout{2000U});
    if (!control) {
        return Result<Px4TsRunResult>::failure(control.error());
    }

    std::uint64_t lease = 0U;
    std::array<std::uint8_t, kNonceLength> nonce{};
    Error primary = Error::OK;
    bool lease_acquired = false;
    bool stream_started = false;
    auto acquire = request_typed(*control.value(), MessageType::ACQUIRE,
                                 AcquireRequestPayload{arguments.receiver}, Timeout{4000U});
    if (!acquire) return Result<Px4TsRunResult>::failure(acquire.error());
    const auto acquire_payload = decode_acquire_response_payload(
        ByteView{acquire.value().payload.data(), acquire.value().payload.size()});
    if (!acquire_payload || acquire_payload.value().lease_id == 0U) {
        return Result<Px4TsRunResult>::failure(Error::PROTOCOL_ERROR);
    }
    lease = acquire_payload.value().lease_id;
    nonce = acquire_payload.value().nonce;
    lease_acquired = true;

    const TuneRequestPayload tune{lease, arguments.system, arguments.frequency_khz,
                                  arguments.stream_id, arguments.slot,
                                  arguments.bandwidth_hz, arguments.lnb_voltage,
                                  arguments.tune_timeout_ms};
    const auto tuned = request_typed(*control.value(), MessageType::TUNE, tune,
                                     Timeout{arguments.tune_timeout_ms + 2000U});
    if (!tuned) {
        primary = tuned.error();
    } else {
        const auto response = decode_tune_response_payload(
            ByteView{tuned.value().payload.data(), tuned.value().payload.size()});
        if (!response || response.value().locked == 0U) primary = Error::NOT_READY;
    }

    if (primary == Error::OK) {
        const auto started = request_typed(*control.value(), MessageType::START_STREAM,
                                           LeaseRequestPayload{lease}, Timeout{4000U});
        if (!started) primary = started.error();
        else stream_started = true;
    }

    SocketStream data;
    if (primary == Error::OK) {
        auto connected = SocketStream::connect(stream_endpoint, Timeout{2000U});
        if (!connected) primary = connected.error();
        else data = std::move(connected.value());
    }

    ConnectionStateMachine data_state(ConnectionRole::stream_client);
    std::array<std::uint8_t, kFrameHeaderSize + 64U> attach_frame{};
    std::array<std::uint8_t, kNonceLength + sizeof(std::uint64_t)> attach_payload{};
    for (unsigned int bit = 0U; bit < 8U; ++bit)
        attach_payload[bit] = static_cast<std::uint8_t>(lease >> (8U * bit));
    std::memcpy(attach_payload.data() + sizeof(std::uint64_t), nonce.data(), nonce.size());
    const FrameHeader attach_header{kProtocolMajor, kProtocolMinor, MessageType::ATTACH_STREAM,
                                    MessageKind::request, 0U,
                                    static_cast<std::uint32_t>(attach_payload.size())};
    const auto encoded_attach = encode_frame(
        attach_header, ByteView{attach_payload.data(), attach_payload.size()},
        MutableByteView{attach_frame.data(), attach_frame.size()});
    if (primary == Error::OK && !encoded_attach) primary = encoded_attach.error();
    if (primary == Error::OK) {
        const auto decoded = decode_frame(ByteView{attach_frame.data(), encoded_attach.value()});
        if (!decoded) primary = decoded.error();
        else if (!data_state.process_outbound(decoded.value())) primary = Error::PROTOCOL_ERROR;
    }
    if (primary == Error::OK) {
        const auto written = data.write_frame(ByteView{attach_frame.data(), encoded_attach.value()},
                                              Timeout{2000U});
        if (!written) {
            primary = written.error();
        }
    }

    std::uint64_t packets_written = 0U;
    std::uint64_t expected_sequence = 0U;
    bool limit_reached = false;
    bool received_end = false;
    StreamEndEventPayload stream_end{};
    Error stream_failure = Error::OK;
    CountersPayload stop_counters{};
    bool stop_counters_valid = false;
    bool stop_requested_by_client = false;
    bool duration_started = false;
    std::uint64_t duration_started_ms = 0U;
    bool progress_started = false;
    std::uint64_t last_progress_ms = 0U;
    if (primary == Error::OK) {
        // Start the first-TS deadline at the completed ATTACH write.  A
        // missing ATTACH response must not leave the read loop unbounded;
        // a successful ATTACH response resets this baseline below.
        progress_started = true;
        last_progress_ms = clock.monotonic_ms();
    }
    std::array<std::uint8_t, 8192U> read_buffer{};
    std::array<std::uint8_t, kFrameHeaderSize + kMaxTsDataPayload> framing_storage{};
    StreamFramer framer(MutableByteView{framing_storage.data(), framing_storage.size()});
    StreamReader reader(data_state, output, arguments, packets_written, expected_sequence,
                        limit_reached, received_end, stream_end, stream_failure,
                        result_failure_kind, stop_requested_by_client, signal,
                        clock, duration_started, duration_started_ms,
                        progress_started, last_progress_ms);
    if (primary == Error::OK) {
        while (!received_end && primary == Error::OK) {
            const std::uint64_t now = clock.monotonic_ms();
            const std::uint64_t elapsed =
                duration_started && now >= duration_started_ms ?
                    now - duration_started_ms : 0U;
            const bool duration_done = arguments.duration_set && duration_started &&
                                       elapsed >= arguments.duration_seconds * 1000U;
            if (signal.stop_requested()) {
                stop_requested_by_client = true;
                break;
            }
            if (duration_done) {
                stop_requested_by_client = true;
                break;
            }
            if (limit_reached) {
                stop_requested_by_client = true;
                break;
            }
            const auto read = data.read_frames(MutableByteView{read_buffer.data(), read_buffer.size()},
                                               framer, reader, Timeout{50U});
            if (!read && read.error() != Error::TIMEOUT) {
                primary = read.error();
                if (read.error() == Error::PROTOCOL_ERROR &&
                    stream_failure == Error::OK &&
                    failed_frame_is_ts_data(framer, framing_storage)) {
                    result_failure_kind = Px4TsFailureKind::ts_integrity;
                }
            }
            else if (!read && read.error() == Error::TIMEOUT) clock.sleep_ms(10U);
            if (stream_failure != Error::OK) primary = stream_failure;
            if (primary == Error::OK) {
                // Re-evaluate requested completion after the read.  A frame
                // can make duration/packet completion true at the same instant
                // as starvation; requested completion wins deterministically.
                const std::uint64_t post_read_now = clock.monotonic_ms();
                const bool duration_done_now =
                    arguments.duration_set && duration_started &&
                    post_read_now >= duration_started_ms &&
                    post_read_now - duration_started_ms >=
                        arguments.duration_seconds * 1000U;
                if (signal.stop_requested() || duration_done_now || limit_reached) {
                    stop_requested_by_client = true;
                    break;
                }
                if (ts_progress_expired(post_read_now, progress_started,
                                        last_progress_ms)) {
                    stream_failure = Error::TIMEOUT;
                    result_failure_kind = Px4TsFailureKind::ts_integrity;
                    primary = stream_failure;
                }
            }
        }
        if (signal.stop_requested()) stop_requested_by_client = true;
    }
    // A successful START_STREAM always receives one idempotent STOP attempt,
    // even when stream connection or ATTACH setup failed before the read loop.
    if (stream_started) {
        const auto stopped = request_typed(*control.value(), MessageType::STOP_STREAM,
                                           LeaseRequestPayload{lease}, Timeout{4000U});
        if (stopped) {
            const auto decoded = decode_counters_payload(
                ByteView{stopped.value().payload.data(), stopped.value().payload.size()});
            if (!decoded) {
                if (primary == Error::OK) primary = Error::PROTOCOL_ERROR;
            } else {
                stop_counters = decoded.value();
                stop_counters_valid = true;
            }
        } else if (stopped.error() != Error::NOT_FOUND && primary == Error::OK) {
            primary = stopped.error();
        }
    }
    if (stream_started && data.valid() && !received_end) {
        // STREAM_END is required after a successful STOP.  A naturally
        // stopped daemon is classified as a disconnect unless the caller
        // had already reached a requested limit or requested signal stop.
        const std::uint64_t deadline = clock.monotonic_ms() + 4000U;
        while (!received_end && clock.monotonic_ms() < deadline) {
            const auto read = data.read_frames(
                MutableByteView{read_buffer.data(), read_buffer.size()}, framer, reader,
                Timeout{50U});
            if (!read && read.error() != Error::TIMEOUT) {
                if (primary == Error::OK) primary = read.error();
                break;
            }
            if (!read) clock.sleep_ms(10U);
            if (stream_failure != Error::OK) {
                if (primary == Error::OK) primary = stream_failure;
                break;
            }
        }
        if (primary == Error::OK && !received_end) primary = Error::TIMEOUT;
    }

    if (data.valid()) data.close();
    if (lease_acquired) {
        const auto released = request_typed(*control.value(), MessageType::RELEASE,
                                            LeaseRequestPayload{lease}, Timeout{4000U});
        if (!released && primary == Error::OK) primary = released.error();
    }
    if (diagnostics != nullptr) {
        diagnostics->stop_counters_valid = stop_counters_valid;
        diagnostics->stop_counters = stop_counters;
        diagnostics->stream_end_valid = received_end;
        diagnostics->stream_end = stream_end;
        diagnostics->packets_written = packets_written;
    }
    if (primary != Error::OK) return Result<Px4TsRunResult>::failure(primary);
    if (!received_end || !safe_counter_bytes(stream_end.counters))
        return Result<Px4TsRunResult>::failure(Error::PROTOCOL_ERROR);
    if (stop_counters_valid && !same_counters(stop_counters, stream_end.counters))
        return Result<Px4TsRunResult>::failure(Error::PROTOCOL_ERROR);
    if (stream_end.error_code != ErrorCode::OK) {
        const auto code = stream_end.error_code;
        if (code == ErrorCode::PROTOCOL_ERROR) {
            // STREAM_END carries the daemon's terminal stream-integrity
            // classification.  A malformed IPC frame/state failure is
            // classified at the read/decoder boundary and remains IPC.
            result_failure_kind = Px4TsFailureKind::ts_integrity;
        }
        return Result<Px4TsRunResult>::failure(
            code == ErrorCode::SLOW_CONSUMER ? Error::SLOW_CONSUMER :
            code == ErrorCode::USB_IO ? Error::USB_IO :
            code == ErrorCode::DISCONNECTED ? Error::DISCONNECTED :
            code == ErrorCode::PROTOCOL_ERROR ? Error::PROTOCOL_ERROR : Error::INTERNAL);
    }
    if (has_ts_integrity_errors(stream_end.counters)) {
        result_failure_kind = Px4TsFailureKind::ts_integrity;
        return Result<Px4TsRunResult>::failure(Error::PROTOCOL_ERROR);
    }
    if (arguments.packet_count_set && packets_written != arguments.packet_count)
        return Result<Px4TsRunResult>::failure(Error::PROTOCOL_ERROR);
    if (stream_end.counters.packets < packets_written || stream_end.counters.bytes < packets_written * 188U)
        return Result<Px4TsRunResult>::failure(Error::PROTOCOL_ERROR);
    if (!stop_requested_by_client &&
        ((!arguments.packet_count_set && !arguments.duration_set) ||
         (arguments.packet_count_set && !limit_reached)))
        return Result<Px4TsRunResult>::failure(Error::DISCONNECTED);
    return Result<Px4TsRunResult>::success(
        Px4TsRunResult{stream_end.counters, signal.stop_requested()});
}

}  // namespace px4::userland::cli
