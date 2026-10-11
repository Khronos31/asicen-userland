#ifndef ASICEN_USERLAND_LOADER_FIRMWARE_H
#define ASICEN_USERLAND_LOADER_FIRMWARE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "asicen/device_profile.h"

namespace asicen {

struct LoaderFirmwareStage {
    std::uint16_t blob_offset;
    std::uint16_t length;
    std::uint8_t final_request;
};

struct LoaderTransfer {
    std::uint8_t request;
    std::uint16_t value;
    std::uint16_t index;
    std::uint16_t length;
    std::uint16_t blob_offset;
};

constexpr std::uint8_t kLoaderRequestType = 0x40;
constexpr std::uint16_t kLoaderFirmwareStartAddress = 0x5399;
constexpr std::uint16_t kLoaderFirmwareChunkSize = 0x0200;
constexpr std::size_t kLoaderFirmwareBlobSize = 0x4000;
constexpr std::uint8_t kLoaderWriteRequest = 0xab;
constexpr std::uint8_t kLoaderFinishRequest = 0xac;

struct LoaderFirmwareManifest {
    const char* name;
    std::size_t size;
    std::uint16_t start_address;
    std::array<std::uint8_t, 32> sha256;
};

// The historical Linux package supplies one image for S3U, S3U2, W3U2 and
// original W3U3. Its loader IDs do not identify which runtime model is attached.
// V2 requires the separately verified Windows image and its own start address.
const LoaderFirmwareManifest& loader_firmware_manifest() noexcept;
// Returns null for unknown models. The no-argument overload is the legacy image.
const LoaderFirmwareManifest* loader_firmware_manifest(ModelId model) noexcept;
bool loader_firmware_supports_model(ModelId model) noexcept;

// Computes SHA-256 without retaining or logging the firmware bytes. A null
// input is permitted only for an empty input; out must not be null.
bool loader_firmware_fingerprint(const std::uint8_t* data, std::size_t length,
                                 std::array<std::uint8_t, 32>* out) noexcept;

const std::array<LoaderFirmwareStage, 4>& loader_firmware_stages();
std::vector<LoaderTransfer> build_loader_transfer_plan();
// Uses this model's verified image start address; unknown models return no plan.
std::vector<LoaderTransfer> build_loader_transfer_plan(ModelId model);

enum class LoaderFirmwareRead {
    Ok,
    WrongSize,
    FingerprintMismatch,
    UnsupportedModel,
};

// Explicit runtime-model selection is mandatory: never guess from a loader
// VID/PID, and recheck the actual runtime VID/PID and port after uploading.
// File loading uses FirmwareProvider; this validates an in-memory image at
// the hardware boundary without creating another file-reading contract.
LoaderFirmwareRead validate_loader_firmware(ModelId model, const std::uint8_t* data,
                                            std::size_t length) noexcept;

} // namespace asicen

#endif // ASICEN_USERLAND_LOADER_FIRMWARE_H
