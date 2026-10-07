#include "asicen/device_profile.h"

namespace asicen {
namespace {

constexpr DeviceProfile kProfiles[] = {
    {0x0b06, 0x0001, "PX-S3U", 1, true},
    {0x0b06, 0x0003, "PX-S3U2", 2, false},
    {0x0b06, 0x0004, "PX-W3U2", 4, false},
    {0x0b06, 0x0005, "PX-W3U3", 4, false},
    {0x0b06, 0x0006, "PX-W3U3 V2", 4, false},
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
