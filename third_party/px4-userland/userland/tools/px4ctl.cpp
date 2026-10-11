// SPDX-License-Identifier: GPL-2.0-only
// ASICEN modification, 2026-10-10: ASICEN command spelling and serial-free instance selection.
// The original license notice is unchanged.
#include "px4/control_client.h"
#include "px4ctl_format.h"

#if defined(_WIN32)
#include "px4_windows_args.h"
#endif

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

enum class Command : std::uint8_t {
    none,
    list,
    status,
    card_status,
    card_atr,
    card_reset,
    card_apdu,
};

struct Arguments final {
    bool valid = false;
    bool help = false;
    bool group = false;
    std::string device;
    std::string instance;
    std::string runtime_directory;
    Command command = Command::none;
    std::vector<std::uint8_t> apdu;
    std::uint32_t repeat = 1U;
    std::string_view error;
};

Arguments invalid(std::string_view error) noexcept
{
    Arguments result;
    result.error = error;
    return result;
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

int hex_nibble(char value) noexcept
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parse_hex(std::string_view value, std::vector<std::uint8_t>& output) noexcept
{
    output.clear();
    if (value.empty()) return false;
    std::size_t offset = 0U;
    while (offset < value.size()) {
        if (output.size() == kMaxCardPayload || offset + 2U > value.size()) {
            output.clear();
            return false;
        }
        const int high = hex_nibble(value[offset]);
        const int low = hex_nibble(value[offset + 1U]);
        if (high < 0 || low < 0) {
            output.clear();
            return false;
        }
        output.push_back(static_cast<std::uint8_t>((high << 4) | low));
        offset += 2U;
        if (offset == value.size()) break;
        if (value[offset++] != ':' || offset == value.size()) {
            output.clear();
            return false;
        }
    }
    return !output.empty();
}

bool parse_repeat(std::string_view value, std::uint32_t& output) noexcept
{
    if (value.empty()) return false;
    std::uint32_t parsed = 0U;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed < 1U || parsed > 100000U) {
        return false;
    }
    output = parsed;
    return true;
}

Command parse_command(std::string_view value) noexcept
{
    if (value == "list") return Command::list;
    if (value == "status") return Command::status;
    if (value == "card-status") return Command::card_status;
    if (value == "card-atr") return Command::card_atr;
    if (value == "card-reset") return Command::card_reset;
    if (value == "card-apdu") return Command::card_apdu;
    return Command::none;
}

Arguments parse_arguments(int argc, const char* const* argv) noexcept
{
    if (argc < 1 || argv == nullptr) return invalid("invalid argument vector");
    Arguments result;
    bool have_device = false;
    bool have_instance = false;
    bool have_runtime = false;
    bool have_command = false;
    bool have_repeat = false;
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
        if (option == "--device" || option == "--instance" ||
            option == "--runtime-dir" ||
            option == "--repeat") {
            if (index + 1 >= argc || argv[index + 1] == nullptr) {
                return invalid("option requires a value");
            }
            const std::string_view value(argv[++index]);
            if (option == "--device") {
#if defined(ASICEN_PRODUCT_CLI)
                return invalid("ASICEN has no serial descriptor; use --instance");
#else
                if (have_device) return invalid("duplicate --device");
                have_device = true;
                result.device = value;
#endif
            } else if (option == "--instance") {
                if (have_instance) return invalid("duplicate --instance");
                have_instance = true;
                result.instance = value;
            } else if (option == "--runtime-dir") {
                if (have_runtime) return invalid("duplicate --runtime-dir");
                have_runtime = true;
                result.runtime_directory = value;
            } else {
                if (have_repeat || !parse_repeat(value, result.repeat)) {
                    return invalid("--repeat must be 1..100000");
                }
                have_repeat = true;
            }
            continue;
        }
        if (!have_command) {
            result.command = parse_command(option);
            if (result.command == Command::none) return invalid("unknown command");
            have_command = true;
            if (result.command == Command::card_apdu) {
                if (index + 1 >= argc || argv[index + 1] == nullptr ||
                    !parse_hex(argv[++index], result.apdu)) {
                    return invalid("card-apdu requires 1..4096 colon-separated bytes");
                }
            }
            continue;
        }
        return invalid("unexpected argument");
    }
#if defined(ASICEN_PRODUCT_CLI)
    if (have_device || !have_instance)
        return invalid("ASICEN requires --instance; device serials are unavailable");
#else
    if (have_device == have_instance)
        return invalid("exactly one of --device or --instance is required");
    if (have_device && !valid_serial(result.device))
        return invalid("--device requires a 14-digit base serial or 15-digit serial");
#endif
    if (have_instance && !valid_instance(result.instance))
        return invalid("--instance is invalid");
    if (have_runtime && result.runtime_directory.empty()) {
        return invalid("--runtime-dir must not be empty");
    }
    if (!have_command) return invalid("a command is required");
    if (have_repeat && result.command != Command::card_apdu) {
        return invalid("--repeat is valid only with card-apdu");
    }
    result.valid = true;
    return result;
}

void usage(FILE* output) noexcept
{
    std::fprintf(output,
#if defined(ASICEN_PRODUCT_CLI)
                 "usage: asicenctl --instance TOKEN "
#else
                 "usage: px4ctl (--device BASE_SERIAL | --instance TOKEN) "
#endif
                 "[--runtime-dir PATH] [--group] "
                 "COMMAND\n");
    std::fprintf(output,
                 "commands: list, status, card-status, card-atr, card-reset, "
                 "card-apdu HEX [--repeat N]\n");
}

bool is_card_command(Command command) noexcept
{
    return command == Command::card_status || command == Command::card_atr ||
           command == Command::card_reset || command == Command::card_apdu;
}

int exit_status(Error error, bool card_operation = false) noexcept
{
    switch (error) {
    case Error::OK: return 0;
    case Error::INVALID_ARGUMENT: return 2;
    case Error::NOT_FOUND:
    case Error::NOT_READY:
    case Error::UNSUPPORTED: return 3;
    case Error::BUSY: return 4;
    case Error::TIMEOUT: return 5;
    case Error::VERSION_MISMATCH: return 6;
    case Error::PROTOCOL_ERROR: return card_operation ? 9 : 6;
    case Error::USB_IO:
    case Error::DISCONNECTED: return 7;
    case Error::SLOW_CONSUMER: return 8;
    case Error::NO_CARD:
    case Error::CARD_REMOVED:
    case Error::BUFFER_TOO_SMALL: return 9;
    case Error::FIRMWARE_REJECTED: return 10;
    case Error::INTERNAL: return 70;
    }
    return 70;
}

void print_hex(const char* label, ByteView value) noexcept
{
    std::printf("%s=", label);
    for (std::size_t index = 0U; index < value.size; ++index) {
        std::printf("%s%02x", index == 0U ? "" : ":",
                    static_cast<unsigned int>(value.data[index]));
    }
    std::printf("\n");
}

template <typename Payload>
Result<ControlResponse> typed_request(PosixControlClient& client, MessageType type,
                                      const Payload& payload,
                                      Timeout timeout = Timeout{4000U}) noexcept
{
    std::array<std::uint8_t, kMaxControlPayload> encoded{};
    const auto size = encode_payload(
        payload, MutableByteView{encoded.data(), encoded.size()});
    if (!size) return Result<ControlResponse>::failure(size.error());
    return client.request(type, ByteView{encoded.data(), size.value()}, timeout);
}

Result<ControlResponse> empty_request(PosixControlClient& client,
                                      MessageType type) noexcept
{
    return client.request(type, ByteView{nullptr, 0U}, Timeout{4000U});
}

struct OpenCard final {
    std::uint64_t handle = 0U;
    std::vector<std::uint8_t> atr;
};

Result<OpenCard> open_card(PosixControlClient& client) noexcept
{
    const auto response = typed_request(
        client, MessageType::CARD_CONNECT,
        CardConnectRequestPayload{ShareMode::shared});
    if (!response) return Result<OpenCard>::failure(response.error());
    const auto decoded = decode_card_connect_response_payload(
        ByteView{response.value().payload.data(), response.value().payload.size()});
    if (!decoded) return Result<OpenCard>::failure(decoded.error());
    OpenCard card;
    card.handle = decoded.value().card_handle;
    if (decoded.value().atr.size != 0U) {
        card.atr.assign(decoded.value().atr.data,
                        decoded.value().atr.data + decoded.value().atr.size);
    }
    return Result<OpenCard>::success(std::move(card));
}

Result<void> close_card(PosixControlClient& client, std::uint64_t handle) noexcept
{
    const auto response = typed_request(
        client, MessageType::CARD_DISCONNECT,
        CardDispositionRequestPayload{handle, Disposition::leave});
    return response ? Result<void>::success() : Result<void>::failure(response.error());
}

}  // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    const std::vector<std::string> owned_argv =
        px4::userland::cli::windows_argv_utf8(argc, argv);
    std::vector<const char*> argv_views;
    argv_views.reserve(owned_argv.size());
    for (const std::string& value : owned_argv) argv_views.push_back(value.c_str());
    const Arguments arguments =
        parse_arguments(static_cast<int>(argv_views.size()), argv_views.data());
#else
    const Arguments arguments =
        parse_arguments(argc, const_cast<const char* const*>(argv));
#endif
    if (!arguments.valid) {
        std::fprintf(stderr, "argument error: %.*s\n",
                     static_cast<int>(arguments.error.size()), arguments.error.data());
        usage(stderr);
        return 2;
    }
    if (arguments.help) {
        usage(stdout);
        return 0;
    }

    const char* runtime_directory = arguments.runtime_directory.empty() ?
                                        nullptr : arguments.runtime_directory.c_str();
    const char* instance = arguments.instance.empty() ? arguments.device.c_str() :
                                                       arguments.instance.c_str();
    const EndpointConfig endpoint{
        runtime_directory, instance, kControlEndpointName,
        arguments.group ? EndpointAccess::shared_group : EndpointAccess::private_user};
    auto client = PosixControlClient::connect(
        endpoint, kCapabilityCard, Timeout{2000U});
    if (!client) {
        std::fprintf(stderr, "connect: %s\n", error_string(client.error()));
        return exit_status(client.error());
    }

    Error operation_error = Error::OK;
    if (arguments.command == Command::list) {
        const auto response = empty_request(*client.value(), MessageType::LIST);
        if (!response) {
            operation_error = response.error();
        } else {
            const auto decoded = decode_list_response_payload(
                ByteView{response.value().payload.data(), response.value().payload.size()});
            if (!decoded) {
                operation_error = decoded.error();
            } else {
                const std::string output = tools::format_list(decoded.value());
                std::fwrite(output.data(), 1U, output.size(), stdout);
            }
        }
    } else if (arguments.command == Command::status) {
        const auto response = empty_request(*client.value(), MessageType::STATUS);
        if (!response) {
            operation_error = response.error();
        } else {
            const auto decoded = decode_status_response_payload(
                ByteView{response.value().payload.data(), response.value().payload.size()});
            if (!decoded) operation_error = decoded.error();
            else {
                const std::string output = tools::format_status(decoded.value());
                std::fwrite(output.data(), 1U, output.size(), stdout);
            }
        }
    } else if (arguments.command == Command::card_status) {
        const auto response = empty_request(*client.value(), MessageType::CARD_STATUS);
        if (!response) {
            operation_error = response.error();
        } else {
            const auto decoded = decode_card_status_response_payload(
                ByteView{response.value().payload.data(), response.value().payload.size()});
            if (!decoded) operation_error = decoded.error();
            else {
                std::printf("present=%s initialized=%s reader-generation=%llu\n",
                            decoded.value().present != 0U ? "yes" : "no",
                            decoded.value().initialized != 0U ? "yes" : "no",
                            static_cast<unsigned long long>(decoded.value().reader_generation));
                if (decoded.value().atr.size != 0U) print_hex("atr", decoded.value().atr);
            }
        }
    } else {
        const auto opened = open_card(*client.value());
        if (!opened) {
            operation_error = opened.error();
        } else {
            if (arguments.command == Command::card_atr) {
                print_hex("atr", ByteView{opened.value().atr.data(), opened.value().atr.size()});
            } else if (arguments.command == Command::card_reset) {
                const auto response = typed_request(
                    *client.value(), MessageType::CARD_RESET,
                    CardHandleRequestPayload{opened.value().handle});
                if (!response) operation_error = response.error();
                else {
                    const auto decoded = decode_atr_payload(
                        ByteView{response.value().payload.data(), response.value().payload.size()});
                    if (!decoded) operation_error = decoded.error();
                    else print_hex("atr", decoded.value().atr);
                }
            } else {
                std::vector<std::uint8_t> last_response;
                for (std::uint32_t iteration = 1U; iteration <= arguments.repeat;
                     ++iteration) {
                    const auto response = typed_request(
                        *client.value(), MessageType::CARD_TRANSMIT,
                        CardTransmitRequestPayload{
                            opened.value().handle,
                            ByteView{arguments.apdu.data(), arguments.apdu.size()}});
                    if (!response) {
                        std::fprintf(stderr, "transmit %u/%u: %s\n",
                                     static_cast<unsigned int>(iteration),
                                     static_cast<unsigned int>(arguments.repeat),
                                     error_string(response.error()));
                        operation_error = response.error();
                        break;
                    }
                    const auto decoded = decode_card_transmit_response_payload(
                        ByteView{response.value().payload.data(), response.value().payload.size()});
                    if (!decoded) {
                        operation_error = decoded.error();
                        break;
                    }
                    last_response.clear();
                    if (decoded.value().response.size != 0U) {
                        last_response.assign(decoded.value().response.data,
                                             decoded.value().response.data +
                                                 decoded.value().response.size);
                    }
                }
                if (operation_error == Error::OK) {
                    print_hex("response", ByteView{last_response.data(), last_response.size()});
                    std::printf("response-length=%zu\ntransmit-count=%u\n",
                                last_response.size(),
                                static_cast<unsigned int>(arguments.repeat));
                }
            }
            const auto closed = close_card(*client.value(), opened.value().handle);
            if (!closed && operation_error == Error::OK) operation_error = closed.error();
        }
    }

    if (operation_error != Error::OK) {
#if defined(ASICEN_PRODUCT_CLI)
        std::fprintf(stderr, "asicenctl: %s\n", error_string(operation_error));
#else
        std::fprintf(stderr, "px4ctl: %s\n", error_string(operation_error));
#endif
    }
    return exit_status(operation_error, is_card_command(arguments.command));
}
