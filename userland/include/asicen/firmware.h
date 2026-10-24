// FirmwareImage/FirmwareProvider adapted from px4-userland
// 1a1485d0c3e972e0a47be907edb67949564aa9a7, userland/include/px4/firmware.h.
// ASICEN requires a verified model-specific loader manifest, not an IT930x image.
// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_USERLAND_FIRMWARE_H
#define ASICEN_USERLAND_FIRMWARE_H

#include "px4/transport.h"
#include "asicen/device_profile.h"
#include "asicen/loader_firmware.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asicen {

using px4::userland::ByteView;
using px4::userland::Result;

class FirmwareTestAccess;

class FirmwareImage final {
public:
    ByteView bytes() const noexcept { return ByteView{data_.data(), data_.size()}; }
    const std::uint8_t* data() const noexcept { return data_.data(); }
    std::size_t size() const noexcept { return data_.size(); }
    const LoaderFirmwareManifest* manifest() const noexcept { return manifest_; }

private:
    FirmwareImage() noexcept = default;
    explicit FirmwareImage(std::vector<std::uint8_t>&& data,
                           const LoaderFirmwareManifest* manifest) noexcept
        : data_(std::move(data)), manifest_(manifest)
    {
    }

    friend class FirmwareProvider;
    friend class FirmwareTestAccess;
    template <typename T>
    friend class px4::userland::Result;

    std::vector<std::uint8_t> data_;
    const LoaderFirmwareManifest* manifest_ = nullptr;
};

class FirmwareProvider final {
public:
    explicit FirmwareProvider(std::string_view path) noexcept : path_(path) {}
    FirmwareProvider(std::string_view path, ModelId model) noexcept
        : path_(path), model_(model) {}
    ~FirmwareProvider() noexcept = default;

    FirmwareProvider(const FirmwareProvider&) = default;
    FirmwareProvider& operator=(const FirmwareProvider&) = default;
    FirmwareProvider(FirmwareProvider&&) noexcept = default;
    FirmwareProvider& operator=(FirmwareProvider&&) noexcept = default;

    Result<FirmwareImage> load() const noexcept;
    std::string_view path() const noexcept { return path_; }

private:
    std::string path_;
    std::optional<ModelId> model_;
};

}  // namespace asicen

#endif  // ASICEN_USERLAND_FIRMWARE_H
