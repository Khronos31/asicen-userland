#pragma once

#include <cstddef>
#include <cstdint>

namespace asicen {

struct DeviceProfile {
    std::uint16_t vid;
    std::uint16_t pid;
    const char* model;
    std::uint8_t receiver_count;
    bool combined_isdb_ts;
};

const DeviceProfile* find_profile(std::uint16_t vid, std::uint16_t pid);
const DeviceProfile* profiles();
std::size_t profile_count();

}  // namespace asicen
