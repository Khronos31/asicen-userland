// ASICEN loader manifests and wire stages. The shared SHA-256 implementation
// is adapted from px4-userland 1a1485d0c3e972e0a47be907edb67949564aa9a7.
// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/loader_firmware.h"

#include "sha256.h"

#include <algorithm>
#include <limits>

namespace asicen {

const LoaderFirmwareManifest& loader_firmware_manifest() noexcept
{
    return common_loader_firmware_manifest();
}

const LoaderFirmwareManifest& common_loader_firmware_manifest() noexcept
{
    static constexpr LoaderFirmwareManifest manifest{
        "PLEX Windows shared loader",
        kLoaderFirmwareBlobSize,
        0x55d6,
        {{0x5d, 0xb3, 0x9f, 0xac, 0xda, 0x14, 0x0b, 0xdc, 0xb3, 0xa2, 0x38,
          0xda, 0xd1, 0x0d, 0xa5, 0xaa, 0xc7, 0x52, 0xa1, 0xba, 0x3a, 0xaf,
          0x17, 0xe2, 0x23, 0x9f, 0x8d, 0x79, 0xa0, 0xad, 0x6a, 0x2a}}};
    return manifest;
}

const LoaderFirmwareManifest* loader_firmware_manifest(ModelId model) noexcept
{
    switch (model) {
    case ModelId::S3u:
    case ModelId::S3u2:
    case ModelId::W3u2:
    case ModelId::W3u3:
    case ModelId::W3u3V2:
        return &common_loader_firmware_manifest();
    }
    return nullptr;
}

bool loader_firmware_supports_model(ModelId model) noexcept
{
    return loader_firmware_manifest(model) != nullptr;
}

bool loader_firmware_fingerprint(const std::uint8_t* data, std::size_t length,
                                 std::array<std::uint8_t, 32>* out) noexcept
{
    if (out == nullptr) {
        return false;
    }
    out->fill(0);
    if ((data == nullptr && length != 0) ||
        static_cast<std::uint64_t>(length) > std::numeric_limits<std::uint64_t>::max() / 8U) {
        return false;
    }
    detail::Sha256 sha256;
    sha256.update(data, length);
    *out = sha256.finish();
    return true;
}

LoaderFirmwareRead validate_loader_firmware(ModelId model, const std::uint8_t* data,
                                            std::size_t length) noexcept
{
    const auto* manifest = loader_firmware_manifest(model);
    if (manifest == nullptr) {
        return LoaderFirmwareRead::UnsupportedModel;
    }
    if (length != manifest->size) {
        return LoaderFirmwareRead::WrongSize;
    }
    std::array<std::uint8_t, 32> digest{};
    if (!loader_firmware_fingerprint(data, length, &digest) || digest != manifest->sha256) {
        return LoaderFirmwareRead::FingerprintMismatch;
    }
    return LoaderFirmwareRead::Ok;
}

const std::array<LoaderFirmwareStage, 4>& loader_firmware_stages()
{
    static constexpr std::array<LoaderFirmwareStage, 4> stages{{
        {0x0000, 0x0c00, kLoaderWriteRequest},
        {0x2000, 0x0400, kLoaderWriteRequest},
        {0x2800, 0x1000, kLoaderWriteRequest},
        {0x3800, 0x0800, kLoaderFinishRequest},
    }};
    return stages;
}

std::vector<LoaderTransfer> build_loader_transfer_plan()
{
    return build_loader_transfer_plan(ModelId::W3u3);
}

std::vector<LoaderTransfer> build_common_loader_transfer_plan()
{
    return build_loader_transfer_plan(common_loader_firmware_manifest());
}

std::vector<LoaderTransfer> build_loader_transfer_plan(ModelId model)
{
    const auto* manifest = loader_firmware_manifest(model);
    return manifest == nullptr ? std::vector<LoaderTransfer>{}
                               : build_loader_transfer_plan(*manifest);
}

std::vector<LoaderTransfer> build_loader_transfer_plan(
    const LoaderFirmwareManifest& manifest)
{
    std::vector<LoaderTransfer> result;
    for (const auto& stage : loader_firmware_stages()) {
        std::uint16_t offset = 0;
        while (offset < stage.length) {
            const std::uint16_t remaining = static_cast<std::uint16_t>(stage.length - offset);
            const std::uint16_t chunk =
                std::min<std::uint16_t>(kLoaderFirmwareChunkSize, remaining);
            const bool final_chunk = static_cast<std::uint16_t>(offset + chunk) == stage.length;
            const std::uint8_t request = final_chunk ? stage.final_request : kLoaderWriteRequest;
            const std::uint16_t blob_offset =
                static_cast<std::uint16_t>(stage.blob_offset + offset);
            result.push_back(LoaderTransfer{
                request,
                blob_offset,
                manifest.start_address,
                chunk,
                blob_offset,
            });
            offset = static_cast<std::uint16_t>(offset + chunk);
        }
    }
    return result;
}

} // namespace asicen
