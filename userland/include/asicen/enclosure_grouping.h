#ifndef ASICEN_USERLAND_ENCLOSURE_GROUPING_H
#define ASICEN_USERLAND_ENCLOSURE_GROUPING_H

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
    std::uint8_t address = 0;
    // Read-only parent USB descriptor, not a guess from the child's VID:PID.
    // 05e3:0608 hub ports 1/2 are the approved ASICEN enclosure topology.
    bool parent_genesys_hub = false;
};

// Two runtime functions in a W3U3/W3U2 enclosure are expected to be siblings
// behind the same internal USB hub. Hardware arrival is the acceptance check
// for this grouping rule.
bool same_usb_parent(const UsbFunctionObservation& a, const UsbFunctionObservation& b);

std::vector<std::pair<std::size_t, std::size_t>>
group_dual_function_enclosures(const std::vector<UsbFunctionObservation>& observations,
                               std::uint16_t vid, std::uint16_t pid);

}  // namespace asicen

#endif  // ASICEN_USERLAND_ENCLOSURE_GROUPING_H
