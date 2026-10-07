#include "asicen/loader_firmware.h"

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

}  // namespace asicen
