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

        std::size_t sibling_count = 0;
        std::size_t sibling_index = observations.size();
        for (std::size_t j = 0; j < observations.size(); ++j) {
            if (j == i || used[j] ||
                observations[j].vid != vid || observations[j].pid != pid) {
                continue;
            }
            if (same_usb_parent(observations[i], observations[j])) {
                ++sibling_count;
                sibling_index = j;
            }
        }

        // A physical dual-function enclosure must have exactly one sibling
        // with the same parent. Any group with 3+ matching children is
        // deliberately left unpaired rather than guessed.
        if (sibling_count != 1 || sibling_index <= i) {
            continue;
        }

        // Re-check from the sibling's point of view so an ambiguous parent
        // group cannot be split into a pair on a later outer-loop iteration.
        std::size_t reverse_sibling_count = 0;
        for (std::size_t j = 0; j < observations.size(); ++j) {
            if (j == sibling_index || used[j] ||
                observations[j].vid != vid || observations[j].pid != pid) {
                continue;
            }
            if (same_usb_parent(observations[sibling_index], observations[j])) {
                ++reverse_sibling_count;
            }
        }
        if (reverse_sibling_count != 1) {
            continue;
        }

        used[i] = true;
        used[sibling_index] = true;
        result.emplace_back(i, sibling_index);
    }
    return result;
}

}  // namespace asicen
