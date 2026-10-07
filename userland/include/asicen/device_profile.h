#pragma once

#include <cstddef>
#include <cstdint>

namespace asicen {

struct DeviceProfile {
    std::uint16_t vid;
    std::uint16_t pid;
    const char* model;

    // User-visible simultaneous receiver capacity of the physical enclosure.
    std::uint8_t enclosure_receiver_count;

    // Expected runtime USB functions that make up one physical enclosure.
    // This is a topology expectation, not yet a grouping key.
    std::uint8_t expected_runtime_functions;

    // Historical ASICEN runtime code allocates two local stream/tuner controls
    // per USB function. Some products (notably S3U) expose fewer independent
    // user-visible receivers because T/S share one physical product frontend.
    std::uint8_t local_lane_count;

    bool combined_isdb_ts;
};

const DeviceProfile* find_profile(std::uint16_t vid, std::uint16_t pid);
const DeviceProfile* profiles();
std::size_t profile_count();

}  // namespace asicen
