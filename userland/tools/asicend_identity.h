// SPDX-License-Identifier: GPL-2.0-only
// Location formatting follows px4d_list_format.cpp at
// 1a1485d0c3e972e0a47be907edb67949564aa9a7. ASICEN's missing serial descriptor
// requires a topology identity; re-enumeration must never widen that identity.
#ifndef ASICEN_USERLAND_ASICEND_IDENTITY_H
#define ASICEN_USERLAND_ASICEND_IDENTITY_H

#include "asicen/device_profile.h"
#include "asicen/enclosure_grouping.h"
#include "px4/error.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace asicen::cli {

inline std::string port_string(const UsbFunctionObservation& location)
{
    if (location.bus == 0U || location.port_path.empty()) return {};
    std::string value = std::to_string(location.bus);
    for (std::size_t i = 0U; i < location.port_path.size(); ++i) {
        value.push_back(i == 0U ? '-' : '.');
        value += std::to_string(location.port_path[i]);
    }
    return value;
}

inline std::string topology_identity(const UsbFunctionObservation& observation,
                                      bool dual)
{
    UsbFunctionObservation enclosure = observation;
    if (dual) {
        if (enclosure.port_path.size() < 2U) return {};
        enclosure.port_path.pop_back();
    }
    const std::string port = port_string(enclosure);
    return port.empty() ? std::string{} : "usb-" + port;
}

inline bool runtime_matches_loader(const UsbFunctionObservation& observation,
                                    const DeviceProfile& model, std::uint8_t bus,
                                    const std::vector<std::uint8_t>& port_path) noexcept
{
    return !port_path.empty() && observation.vid == model.vid && observation.pid == model.pid &&
           observation.bus == bus && observation.port_path == port_path;
}

inline bool is_loader_id(std::uint16_t vid, std::uint16_t pid) noexcept
{
    return vid == 0x1738U && (pid == 0x5211U || pid == 0x5216U);
}

// One selected USB function as observed before any firmware transfer. A
// runtime carries the profile of its own VID:PID; a loader carries none.
enum class SelectedState : std::uint8_t { absent, loader, runtime, unknown };

struct SelectedFunction {
    SelectedState state = SelectedState::absent;
    const DeviceProfile* profile = nullptr;
};

inline SelectedFunction classify_selected_function(std::uint16_t vid, std::uint16_t pid) noexcept
{
    if (is_loader_id(vid, pid)) return {SelectedState::loader, nullptr};
    const DeviceProfile* profile = find_profile(vid, pid);
    return profile != nullptr ? SelectedFunction{SelectedState::runtime, profile}
                              : SelectedFunction{SelectedState::unknown, nullptr};
}

// Warm starts claim the runtime the primary already reports and never
// transfer firmware. Cold starts transfer the shared image; their model is
// decided only after re-enumeration, unless a warm primary already decided it.
// Without firmware, a warm primary keeps the existing loader-sibling
// reservation path and a loader primary is unsupported.
enum class StartupKind : std::uint8_t { warm, cold, refused };

struct StartupDecision {
    StartupKind kind = StartupKind::refused;
    const DeviceProfile* profile = nullptr;
    px4::userland::Error error = px4::userland::Error::INVALID_ARGUMENT;
};

inline StartupDecision decide_startup(const SelectedFunction* functions, std::size_t count,
                                      bool have_firmware) noexcept
{
    using px4::userland::Error;
    if (functions == nullptr || count == 0U || count > 2U)
        return {StartupKind::refused, nullptr, Error::INVALID_ARGUMENT};
    bool all_runtime = true;
    for (std::size_t i = 0U; i < count; ++i) {
        if (functions[i].state == SelectedState::unknown)
            return {StartupKind::refused, nullptr, Error::UNSUPPORTED};
        if (functions[i].state == SelectedState::absent && i == 0U)
            return {StartupKind::refused, nullptr, Error::NOT_FOUND};
        all_runtime = all_runtime && functions[i].state == SelectedState::runtime;
    }
    if (functions[0].state == SelectedState::loader) {
        if (!have_firmware) return {StartupKind::refused, nullptr, Error::UNSUPPORTED};
        return {StartupKind::cold, nullptr, Error::OK};
    }
    if (functions[0].profile == nullptr)
        return {StartupKind::refused, nullptr, Error::UNSUPPORTED};
    if (all_runtime || !have_firmware)
        return {StartupKind::warm, functions[0].profile, Error::OK};
    return {StartupKind::cold, functions[0].profile, Error::OK};
}

// Finds the runtime that re-enumerated at one frozen loader position. Only
// that exact bus and port path is considered; other positions are ignored even
// when their VID:PID is known. A loader still at the position is not ready yet,
// and an unknown VID:PID there is refused instead of being guessed.
struct RuntimeAtPosition {
    px4::userland::Error error = px4::userland::Error::NOT_FOUND;
    std::size_t index = 0U;
    const DeviceProfile* profile = nullptr;
};

inline RuntimeAtPosition find_runtime_at_position(
    const std::vector<UsbFunctionObservation>& observations, std::uint8_t bus,
    const std::vector<std::uint8_t>& port_path) noexcept
{
    using px4::userland::Error;
    if (port_path.empty()) return {Error::INVALID_ARGUMENT, 0U, nullptr};
    RuntimeAtPosition found{};
    std::size_t matches = 0U;
    bool unknown = false;
    for (std::size_t i = 0U; i < observations.size(); ++i) {
        const UsbFunctionObservation& observation = observations[i];
        if (observation.bus != bus || observation.port_path != port_path) continue;
        const SelectedFunction function =
            classify_selected_function(observation.vid, observation.pid);
        if (function.state == SelectedState::unknown) unknown = true;
        if (function.state != SelectedState::runtime) continue;
        found = {Error::OK, i, function.profile};
        ++matches;
    }
    if (matches > 1U) return {Error::BUSY, 0U, nullptr};
    if (unknown) return {Error::UNSUPPORTED, 0U, nullptr};
    if (matches == 0U) return {Error::NOT_FOUND, 0U, nullptr};
    return found;
}

}  // namespace asicen::cli

#endif  // ASICEN_USERLAND_ASICEND_IDENTITY_H
