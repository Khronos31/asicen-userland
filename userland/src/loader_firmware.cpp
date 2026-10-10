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
    // Static provenance: the single FirmBin symbol in the 2013 PLEX Linux
    // loader.ko is identical in both architecture packages. No blob is embedded.
    static constexpr LoaderFirmwareManifest manifest{
        "PLEX Linux 130109 shared loader",
        kLoaderFirmwareBlobSize,
        kLoaderFirmwareStartAddress,
        {{0xb4, 0x5d, 0x51, 0x02, 0x00, 0xa1, 0x69, 0x0b, 0x3c, 0xa3, 0x58,
          0xd9, 0x3d, 0xe1, 0x3f, 0x40, 0xe1, 0xd3, 0x56, 0x7b, 0x66, 0x3c,
          0x17, 0xe7, 0x73, 0x34, 0x9a, 0xd9, 0x6f, 0x59, 0x7a, 0xa8}}};
    return manifest;
}

const LoaderFirmwareManifest* loader_firmware_manifest(ModelId model) noexcept
{
    // The identical x64 Windows loaders in the W3U2, W3U3 and W3U3 V2
    // packages decode a different static image. V2 is assigned that image
    // only; its firmware compatibility does not imply frontend support.
    static constexpr LoaderFirmwareManifest windows{
        "PLEX Windows shared loader",
        kLoaderFirmwareBlobSize,
        0x55d6,
        {{0x5d, 0xb3, 0x9f, 0xac, 0xda, 0x14, 0x0b, 0xdc, 0xb3, 0xa2, 0x38,
          0xda, 0xd1, 0x0d, 0xa5, 0xaa, 0xc7, 0x52, 0xa1, 0xba, 0x3a, 0xaf,
          0x17, 0xe2, 0x23, 0x9f, 0x8d, 0x79, 0xa0, 0xad, 0x6a, 0x2a}}};
    switch (model) {
    case ModelId::S3u:
    case ModelId::S3u2:
    case ModelId::W3u2:
    case ModelId::W3u3:
        return &loader_firmware_manifest();
    case ModelId::W3u3V2:
        return &windows;
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

std::vector<LoaderTransfer> build_loader_transfer_plan(ModelId model)
{
    std::vector<LoaderTransfer> result;
    const auto* manifest = loader_firmware_manifest(model);
    if (manifest == nullptr) {
        return result;
    }
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
                manifest->start_address,
                chunk,
                blob_offset,
            });
            offset = static_cast<std::uint16_t>(offset + chunk);
        }
    }
    return result;
}

} // namespace asicen
