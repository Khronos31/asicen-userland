#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

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

const std::array<LoaderFirmwareStage, 4>& loader_firmware_stages();
std::vector<LoaderTransfer> build_loader_transfer_plan();

}  // namespace asicen
