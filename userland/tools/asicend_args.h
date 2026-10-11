// SPDX-License-Identifier: GPL-2.0-only
// Adapted from px4-userland px4d_args.cpp at 1a1485d0c3e972e0a47be907edb67949564aa9a7.
// ASICEN has no serial descriptor; explicit instances and observed USB topology
// replace serial selection. Model and diagnostic options describe its adapter.
#ifndef ASICEN_USERLAND_ASICEND_ARGS_H
#define ASICEN_USERLAND_ASICEND_ARGS_H

#include "asicen/device_profile.h"
#include "asicen/satellite_tune.h"
#include "px4/identity.h"
#include "px4/error.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace asicen::cli {

struct DaemonArguments final {
    bool valid = false;
    bool help = false;
    bool list = false;
    bool list_json = false;
    bool models = false;
    std::string_view error;
    const DeviceProfile* expected_model = nullptr;
    std::array<std::string, 2U> usb_paths;
    std::size_t usb_path_count = 0U;
    std::array<int, 2U> file_descriptors{};
    std::size_t file_descriptor_count = 0U;
    std::string runtime_directory;
    std::string instance;
    bool probe_satellite = false;
    bool probe_card = false;
    bool card_only = false;
    bool have_satellite_slot = false;
    std::uint32_t satellite_rf_khz = 0U;
    std::uint8_t satellite_slot = 0U;
    bool allow_lnb_power = false;
    bool group = false;
    bool exit_on_stdin_eof = false;
    std::string firmware;
};

inline int exit_status(px4::userland::Error error) noexcept
{
    using px4::userland::Error;
    switch (error) {
    case Error::OK: return 0;
    case Error::INVALID_ARGUMENT: return 2;
    case Error::NOT_FOUND:
    case Error::NOT_READY:
    case Error::NO_CARD:
    case Error::UNSUPPORTED: return 3;
    case Error::BUSY: return 4;
    case Error::TIMEOUT: return 5;
    case Error::VERSION_MISMATCH:
    case Error::PROTOCOL_ERROR: return 6;
    case Error::USB_IO:
    case Error::DISCONNECTED: return 7;
    case Error::SLOW_CONSUMER: return 8;
    case Error::CARD_REMOVED:
    case Error::BUFFER_TOO_SMALL: return 9;
    case Error::FIRMWARE_REJECTED: return 10;
    case Error::INTERNAL: return 70;
    }
    return 70;
}

inline bool parse_unsigned_decimal(std::string_view value, std::uint64_t maximum,
                                   std::uint64_t* output) noexcept
{
    if (value.empty() || output == nullptr) return false;
    std::uint64_t parsed = 0U;
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed > maximum) return false;
    *output = parsed;
    return true;
}

inline bool parse_fd(std::string_view value, int& output) noexcept
{
    if (value.empty()) return false;
    std::uint64_t parsed = 0U;
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    output = static_cast<int>(parsed);
    return true;
}

inline bool parse_usb_path_number(std::string_view value) noexcept
{
    if (value.empty()) return false;
    std::uint16_t parsed = 0U;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
           parsed >= 1U && parsed <= 255U;
}

inline bool valid_usb_path(std::string_view value) noexcept
{
    const std::size_t colon = value.find(':');
    if (colon != std::string_view::npos) {
        return value.find(':', colon + 1U) == std::string_view::npos &&
               parse_usb_path_number(value.substr(0U, colon)) &&
               parse_usb_path_number(value.substr(colon + 1U));
    }
    const std::size_t dash = value.find('-');
    if (dash == std::string_view::npos ||
        !parse_usb_path_number(value.substr(0U, dash))) return false;
    std::size_t start = dash + 1U;
    std::size_t count = 0U;
    while (start < value.size()) {
        const std::size_t dot = value.find('.', start);
        const std::size_t end = dot == std::string_view::npos ? value.size() : dot;
        if (++count > 8U || !parse_usb_path_number(value.substr(start, end - start)))
            return false;
        if (dot == std::string_view::npos) return true;
        start = dot + 1U;
    }
    return false;
}

// Resolution uses the same validated decimal components as the public parser.
inline bool parse_usb_path_component(std::string_view value, std::uint8_t* output) noexcept
{
    if (!parse_usb_path_number(value)) return false;
    std::uint16_t parsed = 0U;
    (void)std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (output != nullptr) *output = static_cast<std::uint8_t>(parsed);
    return true;
}

inline bool parse_bus_address(std::string_view value, std::uint8_t* bus,
                               std::uint8_t* address) noexcept
{
    const std::size_t colon = value.find(':');
    return colon != std::string_view::npos &&
           value.find(':', colon + 1U) == std::string_view::npos &&
           parse_usb_path_component(value.substr(0U, colon), bus) &&
           parse_usb_path_component(value.substr(colon + 1U), address);
}

inline bool parse_bus_port(std::string_view value, std::uint8_t* bus,
                            std::vector<std::uint8_t>* ports) noexcept
{
    const std::size_t dash = value.find('-');
    if (dash == std::string_view::npos ||
        !parse_usb_path_component(value.substr(0U, dash), bus)) return false;
    std::vector<std::uint8_t> parsed;
    std::size_t start = dash + 1U;
    while (start < value.size()) {
        const std::size_t dot = value.find('.', start);
        const std::size_t end = dot == std::string_view::npos ? value.size() : dot;
        std::uint8_t port = 0U;
        if (parsed.size() >= 8U ||
            !parse_usb_path_component(value.substr(start, end - start), &port)) return false;
        parsed.push_back(port);
        if (dot == std::string_view::npos) {
            if (ports != nullptr) *ports = std::move(parsed);
            return true;
        }
        start = dot + 1U;
    }
    return false;
}

inline bool valid_instance(std::string_view value) noexcept
{
    return px4::userland::valid_runtime_instance(value) &&
           !px4::userland::valid_device_instance(value);
}

inline DaemonArguments invalid(std::string_view message) noexcept
{
    DaemonArguments result;
    result.error = message;
    return result;
}

inline bool take_value(int& index, int argc, const char* const* argv,
                       std::string_view& value) noexcept
{
    if (index + 1 >= argc || argv[index + 1] == nullptr) return false;
    value = std::string_view(argv[++index]);
    return !value.empty() && value.rfind("--", 0U) != 0U;
}

inline DaemonArguments parse_daemon_arguments(int argc, const char* const* argv,
                                               bool mock = false) noexcept
{
    if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
        return invalid("invalid argument vector");
    }

    DaemonArguments result;
    if (mock) result.instance = "default";
    bool have_model = false;
    bool have_firmware = false;
    bool have_runtime = false;
    bool have_instance = false;
    bool have_allow_lnb_power = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) return invalid("null argument");
        const std::string_view option(argv[index]);
        if (option == "--help") {
            if (argc != 2) return invalid("--help cannot be combined");
            result.valid = true;
            result.help = true;
            return result;
        }
        if (option == "--list") {
            if (argc != 2) return invalid("--list cannot be combined");
            if (mock) return invalid("unknown argument");
            result.valid = true;
            result.list = true;
            return result;
        }
        if (option == "--list-json") {
            if (argc != 2) return invalid("--list-json cannot be combined");
            if (mock) return invalid("unknown argument");
            result.valid = true;
            result.list_json = true;
            return result;
        }
        if (option == "--models") {
            if (argc != 2) return invalid("--models cannot be combined");
            result.valid = true;
            result.models = true;
            return result;
        }
        if (option == "--group") {
            if (result.group) return invalid("duplicate --group");
            result.group = true;
            continue;
        }
        if (option == "--allow-lnb-power") {
            if (have_allow_lnb_power)
                return invalid("duplicate --allow-lnb-power");
            have_allow_lnb_power = true;
            result.allow_lnb_power = true;
            continue;
        }
#if defined(_WIN32)
        if (option == "--exit-on-stdin-eof") {
            if (result.exit_on_stdin_eof) return invalid("duplicate --exit-on-stdin-eof");
            result.exit_on_stdin_eof = true;
            continue;
        }
#endif
        if (!mock && (option == "--probe-card" || option == "--card-only")) {
            if (result.probe_card || result.probe_satellite || result.card_only)
                return invalid("diagnostic modes cannot be combined");
            result.probe_card = option == "--probe-card";
            result.card_only = option == "--card-only";
            continue;
        }
        if (!mock && (option == "--probe-satellite" || option == "--slot")) {
            std::string_view value;
            if (!take_value(index, argc, argv, value)) {
                return invalid("option requires a value");
            }
            if (option == "--probe-satellite") {
                if (result.probe_card || result.probe_satellite || result.card_only)
                    return invalid("diagnostic modes cannot be combined");
                std::uint64_t frequency = 0U;
                if (!parse_unsigned_decimal(value, std::numeric_limits<std::uint32_t>::max(),
                                            &frequency) ||
                    !is_w3u3_satellite_rf_khz(static_cast<std::uint32_t>(frequency)))
                    return invalid("--probe-satellite is invalid");
                result.probe_satellite = true;
                result.satellite_rf_khz = static_cast<std::uint32_t>(frequency);
            } else {
                if (result.have_satellite_slot) return invalid("duplicate --slot");
                std::uint64_t slot = 0U;
                if (!parse_unsigned_decimal(value, kW3u3SatelliteTsidSlots - 1U, &slot))
                    return invalid("--slot must be 0..7");
                result.have_satellite_slot = true;
                result.satellite_slot = static_cast<std::uint8_t>(slot);
            }
            continue;
        }
        if (mock && (option == "--firmware" || option == "--fd" || option == "--usb-path"))
            return invalid("unknown argument");
        if (option != "--model" && option != "--firmware" &&
            option != "--runtime-dir" && option != "--fd" &&
            option != "--usb-path" && option != "--instance") {
            return invalid("unknown argument");
        }

        std::string_view value;
        if (!take_value(index, argc, argv, value)) {
            return invalid("option requires a value");
        }
        if (option == "--model") {
            if (have_model) return invalid("duplicate --model");
            have_model = true;
            result.expected_model = find_profile_by_model(value);
            if (result.expected_model == nullptr) return invalid("unknown ASICEN model");
        } else if (option == "--instance") {
            if (have_instance) return invalid("duplicate --instance");
            have_instance = true;
            result.instance = value;
        } else if (option == "--usb-path") {
            if (result.usb_path_count >= result.usb_paths.size())
                return invalid("at most two --usb-path values are supported");
            if (!valid_usb_path(value)) return invalid("--usb-path is invalid");
            for (std::size_t path_index = 0U; path_index < result.usb_path_count;
                 ++path_index) {
                if (result.usb_paths[path_index] == value)
                    return invalid("--usb-path values must be distinct");
            }
            result.usb_paths[result.usb_path_count++] = value;
        } else if (option == "--firmware") {
            if (have_firmware) return invalid("duplicate --firmware");
            have_firmware = true;
            result.firmware = value;
        } else if (option == "--runtime-dir") {
            if (have_runtime) return invalid("duplicate --runtime-dir");
            have_runtime = true;
            result.runtime_directory = value;
        } else {
            if (result.file_descriptor_count >= result.file_descriptors.size()) {
                return invalid("at most two --fd values are supported");
            }
            int fd = -1;
            if (!parse_fd(value, fd)) return invalid("--fd is invalid");
            for (std::size_t fd_index = 0U;
                 fd_index < result.file_descriptor_count; ++fd_index) {
                if (result.file_descriptors[fd_index] == fd) {
                    return invalid("--fd values must be distinct");
                }
            }
            result.file_descriptors[result.file_descriptor_count++] = fd;
        }
    }
    if (have_instance && !valid_instance(result.instance))
        return invalid("--instance is invalid");
    if (!mock) {
        if (result.usb_path_count != 0U && result.file_descriptor_count != 0U)
            return invalid("--usb-path and --fd cannot be combined");
        if (result.usb_path_count == 0U && result.file_descriptor_count == 0U)
            return invalid("native mode requires --usb-path");
        if (result.usb_path_count != 0U && !have_instance &&
            !result.probe_card && !result.probe_satellite)
            return invalid("--usb-path requires --instance");
        if (result.have_satellite_slot && !result.probe_satellite)
            return invalid("--slot requires --probe-satellite");
        if (!result.firmware.empty() && result.expected_model == nullptr)
            return invalid("--firmware requires --model");
    }
    if (have_runtime && result.runtime_directory.empty()) {
        return invalid("--runtime-dir must not be empty");
    }
    result.valid = true;
    return result;
}

}  // namespace asicen::cli

#endif  // ASICEN_USERLAND_ASICEND_ARGS_H
