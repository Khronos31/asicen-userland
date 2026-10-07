#include "asicen/loader_firmware.h"

#include <algorithm>

namespace asicen {

const std::array<LoaderFirmwareStage, 4>& loader_firmware_stages() {
    static constexpr std::array<LoaderFirmwareStage, 4> stages{{
        {0x0000, 0x0c00, kLoaderWriteRequest},
        {0x2000, 0x0400, kLoaderWriteRequest},
        {0x2800, 0x1000, kLoaderWriteRequest},
        {0x3800, 0x0800, kLoaderFinishRequest},
    }};
    return stages;
}

std::vector<LoaderTransfer> build_loader_transfer_plan() {
    std::vector<LoaderTransfer> result;
    for (const auto& stage : loader_firmware_stages()) {
        std::uint16_t offset = 0;
        while (offset < stage.length) {
            const std::uint16_t remaining =
                static_cast<std::uint16_t>(stage.length - offset);
            const std::uint16_t chunk =
                std::min<std::uint16_t>(kLoaderFirmwareChunkSize, remaining);
            const bool final_chunk =
                static_cast<std::uint16_t>(offset + chunk) == stage.length;
            const std::uint8_t request =
                final_chunk ? stage.final_request : kLoaderWriteRequest;
            const std::uint16_t blob_offset =
                static_cast<std::uint16_t>(stage.blob_offset + offset);
            result.push_back(LoaderTransfer{
                request,
                blob_offset,
                kLoaderFirmwareStartAddress,
                chunk,
                blob_offset,
            });
            offset = static_cast<std::uint16_t>(offset + chunk);
        }
    }
    return result;
}

}  // namespace asicen
