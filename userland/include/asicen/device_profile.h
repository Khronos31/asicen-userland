#ifndef ASICEN_USERLAND_DEVICE_PROFILE_H
#define ASICEN_USERLAND_DEVICE_PROFILE_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace asicen {

enum class ModelId : std::uint8_t { S3u, S3u2, W3u2, W3u3, W3u3V2 };
enum class FrontendFamily : std::uint8_t { S3u, S3u2, W3u3, Nmi };

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

    // Source-backed dispatch is separate from observed hardware reception.
    // Defaults keep historical seven-field aggregate initializers fail-closed.
    ModelId model_id = ModelId::W3u3;
    FrontendFamily frontend_family = FrontendFamily::W3u3;
    bool source_supported = false;
    const char* model_key = "unknown";
};

const DeviceProfile* find_profile(std::uint16_t vid, std::uint16_t pid);
const DeviceProfile* find_profile(ModelId model);
// Canonical keys are s3u, s3u2, w3u2, w3u3, w3u3-v2; model display names
// (case-insensitive, with optional PX- prefix) are also accepted.
const DeviceProfile* find_profile_by_model(std::string_view model);
const char* model_id_name(ModelId model) noexcept;
const char* frontend_family_name(FrontendFamily family) noexcept;
bool profile_runtime_supported(const DeviceProfile& profile) noexcept;
const DeviceProfile* profiles();
std::size_t profile_count();

}  // namespace asicen

#endif  // ASICEN_USERLAND_DEVICE_PROFILE_H
