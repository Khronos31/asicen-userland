#include "asicen/loader_firmware.h"

#include <algorithm>
#include <fstream>
#include <utility>

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

LoaderFirmwareRead read_loader_firmware_file(const std::string& path,
                                             std::vector<std::uint8_t>* out) {
    if (out == nullptr) {
        return LoaderFirmwareRead::OpenFailed;
    }
    out->clear();

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return LoaderFirmwareRead::OpenFailed;
    }

    std::vector<std::uint8_t> data(kLoaderFirmwareBlobSize + 1);
    file.read(reinterpret_cast<char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    const std::streamsize got = file.gcount();
    if (file.bad() || got < 0) {
        return LoaderFirmwareRead::OpenFailed;
    }
    if (static_cast<std::size_t>(got) != kLoaderFirmwareBlobSize) {
        return LoaderFirmwareRead::WrongSize;
    }
    data.resize(static_cast<std::size_t>(got));

    *out = std::move(data);
    return LoaderFirmwareRead::Ok;
}

}  // namespace asicen
