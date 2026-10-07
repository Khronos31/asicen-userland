#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace asicen {

struct UsbFunctionObservation {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    std::uint8_t bus = 0;
    std::vector<std::uint8_t> port_path;
};

// Two runtime functions in a W3U3/W3U2 enclosure are expected to be siblings
// behind the same internal USB hub. Hardware arrival is the acceptance check
// for this grouping rule.
bool same_usb_parent(const UsbFunctionObservation& a,
                     const UsbFunctionObservation& b);

std::vector<std::pair<std::size_t, std::size_t>> group_dual_function_enclosures(
    const std::vector<UsbFunctionObservation>& observations,
    std::uint16_t vid,
    std::uint16_t pid);

}  // namespace asicen
