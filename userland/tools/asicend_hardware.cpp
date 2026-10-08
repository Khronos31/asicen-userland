// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_stream_session.h"
#include "asicen/enclosure_lock.h"
#include "asicen/card_only_service.h"
#include "asicen/libusb_hardware_backend.h"
#include "asicen/product_profile.h"
#include "asicen/px4_mock_backend.h"
#include "px4/control_server.h"
#include "px4/posix_tuner_nonce.h"

#include <signal.h>
#include <unistd.h>
#include <libusb.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
volatile sig_atomic_t stop_requested = 0;
void signal_handler(int) { stop_requested = 1; }

class Time final : public px4::userland::TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t ms) noexcept override {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
};

struct Options {
    bool hardware = false;
    bool have_primary = false;
    bool have_sibling = false;
    std::uint8_t primary_bus = 0;
    std::uint8_t primary_address = 0;
    std::uint8_t sibling_bus = 0;
    std::uint8_t sibling_address = 0;
    std::string primary_port;
    std::string sibling_port;
    std::string runtime_dir;
    std::string instance = "default";
    bool probe_satellite = false;
    bool probe_card = false;
    bool card_only = false;
    bool have_satellite_slot = false;
    std::uint32_t satellite_rf_khz = 0;
    std::uint8_t satellite_slot = 0;
};

bool parse_unsigned_decimal(const std::string& text, std::uint64_t maximum,
                            std::uint64_t* value) {
    if (value == nullptr || text.empty() ||
        !std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        })) return false;
    try {
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(text, &consumed, 10);
        if (consumed != text.size() || parsed > maximum) return false;
        *value = static_cast<std::uint64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_port(const std::string& text, std::uint8_t bus,
                std::vector<std::uint8_t>* ports) {
    if (ports == nullptr || !asicen::parse_port_path(text)) return false;
    const auto dash = text.find('-');
    if (dash == std::string::npos) return false;
    unsigned parsed_bus = 0;
    try { parsed_bus = static_cast<unsigned>(std::stoul(text.substr(0, dash))); }
    catch (...) { return false; }
    if (parsed_bus != bus) return false;
    std::size_t offset = dash + 1U;
    while (offset < text.size()) {
        const auto end = text.find('.', offset);
        const auto token = text.substr(offset, end == std::string::npos
                                                   ? std::string::npos : end - offset);
        unsigned value = 0;
        try { value = static_cast<unsigned>(std::stoul(token)); }
        catch (...) { return false; }
        if (value == 0U || value > 255U) return false;
        ports->push_back(static_cast<std::uint8_t>(value));
        if (end == std::string::npos) break;
        offset = end + 1U;
    }
    return !ports->empty();
}

bool valid_instance(const std::string& value) {
    if (value.empty() || value.size() > 80U || value == "." || value == "..") return false;
    const bool serial_shaped = (value.size() == 14U || value.size() == 15U) &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        });
    if (serial_shaped) return false;
    for (const unsigned char c : value) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.') continue;
        return false;
    }
    return true;
}

bool parse_arguments(int argc, char** argv, Options* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--hardware") out->hardware = true;
        else if (arg == "--primary" && i + 1 < argc) {
            out->have_primary = asicen::parse_usb_location(
                argv[++i], &out->primary_bus, &out->primary_address);
            if (!out->have_primary) return false;
        } else if (arg == "--sibling" && i + 1 < argc) {
            out->have_sibling = asicen::parse_usb_location(
                argv[++i], &out->sibling_bus, &out->sibling_address);
            if (!out->have_sibling) return false;
        } else if (arg == "--primary-port" && i + 1 < argc) {
            out->primary_port = argv[++i];
        } else if (arg == "--sibling-port" && i + 1 < argc) {
            out->sibling_port = argv[++i];
        } else if (arg == "--runtime-dir" && i + 1 < argc) {
            out->runtime_dir = argv[++i];
        } else if (arg == "--instance" && i + 1 < argc) {
            out->instance = argv[++i];
        } else if (arg == "--probe-satellite" && i + 1 < argc) {
            if (out->probe_satellite || out->probe_card) return false;
            std::uint64_t parsed = 0;
            if (!parse_unsigned_decimal(argv[++i],
                    std::numeric_limits<std::uint32_t>::max(), &parsed) ||
                !asicen::is_w3u3_satellite_rf_khz(static_cast<std::uint32_t>(parsed)))
                return false;
            out->probe_satellite = true;
            out->satellite_rf_khz = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--probe-card") {
            if (out->probe_satellite || out->probe_card || out->card_only) return false;
            out->probe_card = true;
        } else if (arg == "--card-only") {
            if (out->probe_satellite || out->probe_card || out->card_only) return false;
            out->card_only = true;
        } else if (arg == "--slot" && i + 1 < argc) {
            if (out->have_satellite_slot) return false;
            std::uint64_t parsed = 0;
            if (!parse_unsigned_decimal(argv[++i],
                    asicen::kW3u3SatelliteTsidSlots - 1U, &parsed)) return false;
            out->have_satellite_slot = true;
            out->satellite_slot = static_cast<std::uint8_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            std::puts("usage: asicend --hardware --primary BUS:ADDR --primary-port BUS-PORT "
                      "--sibling BUS:ADDR --sibling-port BUS-PORT "
                      "[--runtime-dir PATH] [--instance TOKEN]\n"
                      "       asicend --hardware --primary BUS:ADDR --primary-port BUS-PORT "
                      "--sibling BUS:ADDR --sibling-port BUS-PORT "
                      "--probe-satellite RF_KHZ [--slot 0..7]\n"
                      "       asicend --hardware --primary BUS:ADDR --primary-port BUS-PORT "
                      "--sibling BUS:ADDR --sibling-port BUS-PORT --probe-card\n"
                      "       asicend --hardware --primary BUS:ADDR --primary-port BUS-PORT "
                      "--sibling BUS:ADDR --sibling-port BUS-PORT --card-only "
                      "[--runtime-dir PATH] [--instance TOKEN]");
            std::exit(0);
        } else return false;
    }
    return out->hardware && out->have_primary && out->have_sibling &&
           !out->primary_port.empty() && !out->sibling_port.empty() &&
            out->runtime_dir.size() < 400U && valid_instance(out->instance) &&
           (!out->have_satellite_slot || out->probe_satellite) &&
           (!out->card_only || (!out->probe_satellite && !out->probe_card));
}

int lock_runtime() {
    return asicen::acquire_enclosure_lock("/tmp/asicen-userland-enclosure.lock");
}
}  // namespace

int run_asicend_hardware(int argc, char** argv) {
    Options options;
    if (!parse_arguments(argc, argv, &options)) {
        std::fprintf(stderr, "usage: asicend --hardware --primary BUS:ADDR --primary-port BUS-PORT "
                             "--sibling BUS:ADDR --sibling-port BUS-PORT "
                             "[--runtime-dir PATH] [--instance TOKEN]\n");
        return 2;
    }
    stop_requested = 0;
    struct sigaction action{};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    ::signal(SIGPIPE, SIG_IGN);
    std::vector<std::uint8_t> primary_path, sibling_path;
    if (!parse_port(options.primary_port, options.primary_bus, &primary_path) ||
        !parse_port(options.sibling_port, options.sibling_bus, &sibling_path)) {
        std::fprintf(stderr, "USB port path bus must match selected bus\n");
        return 2;
    }
    if (primary_path == sibling_path || primary_path.back() != 1U ||
        sibling_path.back() != 2U) {
        std::fprintf(stderr, "refusing noncanonical primary/sibling port assignment\n");
        return 2;
    }
    const int lock_fd = lock_runtime();
    if (lock_fd < 0) {
        std::fprintf(stderr, "ASICEN enclosure is already owned or lock path is unsafe\n");
        return 4;
    }
    libusb_context* context = nullptr;
    const int init_rc = libusb_init(&context);
    if (init_rc != 0) {
        std::fprintf(stderr, "libusb_init: %s\n", libusb_error_name(init_rc));
        ::close(lock_fd); return 70;
    }
    int result = 70;
    {
        asicen::LibusbW3u3Hardware hardware(
            context, {options.primary_bus, options.primary_address},
            {options.sibling_bus, options.sibling_address},
            primary_path, sibling_path);
        const auto claimed = hardware.claim();
        if (!claimed) {
            std::fprintf(stderr, "hardware topology/claim refused: %s\n",
                         px4::userland::error_string(claimed.error()));
            result = claimed.error() == px4::userland::Error::BUSY ? 4 : 3;
        } else if (options.probe_satellite) {
            const auto probe = hardware.probe_satellite(
                options.satellite_rf_khz, options.have_satellite_slot,
                options.satellite_slot, &stop_requested);
            std::fprintf(stderr,
                "satellite-probe status=%s rf_khz=%u lock=%u nonempty_slots=%u selected=%u\n",
                asicen::satellite_operation_result_name(probe.result),
                options.satellite_rf_khz, probe.locked ? 1U : 0U,
                static_cast<unsigned>(probe.nonempty_tsid_slots),
                probe.selected_slot ? 1U : 0U);
            result = probe.result == asicen::SatelliteOperationResult::Completed ? 0 :
                     probe.result == asicen::SatelliteOperationResult::Cancelled ? 130 : 70;
            const auto stopped = hardware.shutdown();
            if (!stopped) {
                std::fprintf(stderr, "satellite-probe cleanup failed\n");
                result = 70;
            }
            const auto released = hardware.release();
            if (!released) {
                std::fprintf(stderr, "USB claim release failed\n");
                result = 70;
            }
        } else if (options.probe_card) {
            const auto probe = hardware.probe_card(&stop_requested);
            std::fprintf(stderr,
                "card-probe status=%s atr_valid=%u atr_length=%zu "
                "response_length=%zu sw=%04x\n",
                px4::userland::error_string(probe.error),
                probe.atr_valid ? 1U : 0U, probe.atr_length,
                probe.response_length, probe.status_word);
            result = probe.error == px4::userland::Error::OK ? 0 :
                     probe.error == px4::userland::Error::TIMEOUT ? 130 : 70;
            const auto stopped = hardware.shutdown();
            if (!stopped) {
                std::fprintf(stderr, "card-probe cleanup failed\n");
                result = 70;
            }
            const auto released = hardware.release();
            if (!released) {
                std::fprintf(stderr, "USB claim release failed\n");
                result = 70;
            }
        } else if (options.card_only) {
            std::fprintf(stderr,
                "asicend card-only mode: tuner operations are unsupported\n");
            result = asicen::run_card_only_server(
                hardware, options.runtime_dir.c_str(), options.instance.c_str(),
                &stop_requested);
            const auto stopped = hardware.shutdown();
            if (!stopped) {
                std::fprintf(stderr, "card-only hardware cleanup failed\n");
                result = 70;
            }
            const auto released = hardware.release();
            if (!released) {
                std::fprintf(stderr, "USB claim release failed\n");
                result = 70;
            }
        } else {
            result = asicen::run_live_card_stream_server(
                hardware,
                options.runtime_dir.empty() ? nullptr : options.runtime_dir.c_str(),
                options.instance.c_str(), &stop_requested);
            const auto stopped = hardware.shutdown();
            if (!stopped) {
                std::fprintf(stderr, "asicend hardware cleanup failed: %s\n",
                             px4::userland::error_string(stopped.error()));
                result = 70;
            }
          const auto released = hardware.release();
          if (!released) {
              std::fprintf(stderr, "USB claim release failed\n");
              result = 70;
          }
        }
    }
    libusb_exit(context);
    ::close(lock_fd);
    return result;
}
