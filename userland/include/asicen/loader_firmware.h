#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace asicen {

struct LoaderFirmwareStage {
    std::uint16_t blob_offset;
    std::uint16_t length;
    std::uint8_t final_request;
};

constexpr std::uint16_t kLoaderFirmwareStartAddress = 0x5399;
constexpr std::uint16_t kLoaderFirmwareChunkSize = 0x0200;
constexpr std::size_t kLoaderFirmwareBlobSize = 0x4000;
constexpr std::uint8_t kLoaderWriteRequest = 0xab;
constexpr std::uint8_t kLoaderFinishRequest = 0xac;

const std::array<LoaderFirmwareStage, 4>& loader_firmware_stages();

}  // namespace asicen
