#include "asicen/device_profile.h"

#include <string>

namespace asicen {
namespace {

constexpr DeviceProfile kProfiles[] = {
    // vid, pid, model, enclosure receivers, runtime USB functions, local lanes/function, combined
    // T/S
    {0x0b06, 0x0001, "PX-S3U", 1, 1, 2, true, ModelId::S3u, FrontendFamily::S3u, true, "s3u"},
    {0x0b06, 0x0003, "PX-S3U2", 2, 1, 2, false, ModelId::S3u2, FrontendFamily::S3u2, true, "s3u2"},
    {0x0b06, 0x0004, "PX-W3U2", 4, 2, 2, false, ModelId::W3u2, FrontendFamily::W3u3, true, "w3u2"},
    {0x0b06, 0x0005, "PX-W3U3", 4, 2, 2, false, ModelId::W3u3, FrontendFamily::W3u3, true, "w3u3"},
    {0x0b06, 0x0006, "PX-W3U3 V2", 4, 2, 2, false, ModelId::W3u3V2, FrontendFamily::Nmi, true,
     "w3u3-v2"},
};

}  // namespace

const DeviceProfile* find_profile(std::uint16_t vid, std::uint16_t pid)
{
    for (const auto& profile : kProfiles) {
        if (profile.vid == vid && profile.pid == pid) {
            return &profile;
        }
    }
    return nullptr;
}

const DeviceProfile* find_profile(ModelId model)
{
    for (const auto& profile : kProfiles) {
        if (profile.model_id == model) {
            return &profile;
        }
    }
    return nullptr;
}

const DeviceProfile* find_profile_by_model(std::string_view model)
{
    std::string key;
    key.reserve(model.size());
    for (const unsigned char c : model) {
        key.push_back(c >= 'A' && c <= 'Z'   ? static_cast<char>(c - 'A' + 'a')
                      : c == ' ' || c == '_' ? '-'
                                             : static_cast<char>(c));
    }
    if (key.compare(0, 3, "px-") == 0) {
        key.erase(0, 3);
    }
    if (key == "w3u3v2") {
        key = "w3u3-v2";
    }
    for (const auto& profile : kProfiles) {
        if (key == profile.model_key) {
            return &profile;
        }
    }
    return nullptr;
}

const char* model_id_name(ModelId model) noexcept
{
    const auto* profile = find_profile(model);
    return profile == nullptr ? "unknown" : profile->model_key;
}

const char* frontend_family_name(FrontendFamily family) noexcept
{
    switch (family) {
    case FrontendFamily::S3u:
        return "s3u";
    case FrontendFamily::S3u2:
        return "s3u2";
    case FrontendFamily::W3u3:
        return "w3u3";
    case FrontendFamily::Nmi:
        return "nmi";
    }
    return "unknown";
}

bool profile_runtime_supported(const DeviceProfile& profile) noexcept
{
    const auto* known = find_profile(profile.vid, profile.pid);
    return known != nullptr && known->model_id == profile.model_id &&
           known->frontend_family == profile.frontend_family &&
           known->enclosure_receiver_count == profile.enclosure_receiver_count &&
           known->expected_runtime_functions == profile.expected_runtime_functions &&
           known->local_lane_count == profile.local_lane_count &&
           known->combined_isdb_ts == profile.combined_isdb_ts && known->source_supported &&
           profile.source_supported;
}

const DeviceProfile* profiles()
{
    return kProfiles;
}
std::size_t profile_count()
{
    return sizeof(kProfiles) / sizeof(kProfiles[0]);
}

}  // namespace asicen
