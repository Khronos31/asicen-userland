#include "asicen/device_profile.h"

namespace asicen {
namespace {

constexpr DeviceProfile kProfiles[] = {
    // vid, pid, model, enclosure receivers, runtime USB functions, local lanes/function, combined T/S
    {0x0b06, 0x0001, "PX-S3U",     1, 1, 2, true},
    {0x0b06, 0x0003, "PX-S3U2",    2, 1, 2, false},
    {0x0b06, 0x0004, "PX-W3U2",    4, 2, 2, false},
    {0x0b06, 0x0005, "PX-W3U3",    4, 2, 2, false},
    {0x0b06, 0x0006, "PX-W3U3 V2", 4, 2, 2, false},
};

}  // namespace

const DeviceProfile* find_profile(std::uint16_t vid, std::uint16_t pid) {
    for (const auto& profile : kProfiles) {
        if (profile.vid == vid && profile.pid == pid) {
            return &profile;
        }
    }
    return nullptr;
}

const DeviceProfile* profiles() { return kProfiles; }
std::size_t profile_count() { return sizeof(kProfiles) / sizeof(kProfiles[0]); }

}  // namespace asicen
