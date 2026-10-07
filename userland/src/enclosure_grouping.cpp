#include "asicen/enclosure_grouping.h"

#include <algorithm>

namespace asicen {

bool same_usb_parent(const UsbFunctionObservation& a,
                     const UsbFunctionObservation& b) {
    if (a.bus != b.bus || a.port_path.empty() || b.port_path.empty() ||
        a.port_path.size() != b.port_path.size()) {
        return false;
    }
    if (a.port_path.size() == 1) {
        return false;
    }
    return std::equal(a.port_path.begin(), a.port_path.end() - 1,
                      b.port_path.begin(), b.port_path.end() - 1);
}

std::vector<std::pair<std::size_t, std::size_t>> group_dual_function_enclosures(
    const std::vector<UsbFunctionObservation>& observations,
    std::uint16_t vid,
    std::uint16_t pid) {
    std::vector<std::pair<std::size_t, std::size_t>> result;
    std::vector<bool> used(observations.size(), false);

    for (std::size_t i = 0; i < observations.size(); ++i) {
        if (used[i] || observations[i].vid != vid || observations[i].pid != pid) {
            continue;
        }

        std::optional<std::size_t> match;
        for (std::size_t j = i + 1; j < observations.size(); ++j) {
            if (used[j] || observations[j].vid != vid || observations[j].pid != pid) {
                continue;
            }
            if (!same_usb_parent(observations[i], observations[j])) {
                continue;
            }
            if (match.has_value()) {
                // More than two matching siblings is ambiguous. Leave all of
                // them ungrouped rather than choosing arbitrarily.
                match.reset();
                break;
            }
            match = j;
        }

        if (match.has_value()) {
            used[i] = true;
            used[*match] = true;
            result.emplace_back(i, *match);
        }
    }
    return result;
}

}  // namespace asicen
