// FirmwareImage/FirmwareProvider adapted from px4-userland
// 1a1485d0c3e972e0a47be907edb67949564aa9a7, userland/src/firmware.cpp.
// ASICEN modification: model-specific size/hash policy and no IT930x scatter
// interpretation. Error classes, chunking and SHA processing match; stdio keeps
// read errors distinct from EOF on standard libraries that conflate them.
// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/firmware.h"

#include "asicen/loader_firmware.h"
#include "sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <string>
#include <windows.h>
#endif

namespace asicen {

using px4::userland::Error;
using detail::Sha256;

namespace {

const LoaderFirmwareManifest* matching_manifest(
    const std::optional<ModelId>& model, std::size_t size,
    const std::array<std::uint8_t, 32U>& digest) noexcept
{
    if (model.has_value()) {
        const auto* manifest = loader_firmware_manifest(*model);
        return manifest != nullptr && manifest->size == size && manifest->sha256 == digest
            ? manifest : nullptr;
    }
    const auto& common = common_loader_firmware_manifest();
    return common.size == size && common.sha256 == digest ? &common : nullptr;
}

}  // namespace

Result<FirmwareImage> FirmwareProvider::load() const noexcept
{
    if (path_.empty() || path_.find('\0') != std::string::npos) {
        return Result<FirmwareImage>::failure(Error::INVALID_ARGUMENT);
    }

#if defined(_WIN32)
    // Windows narrow fopen uses the active code page, so a UTF-8 path outside
    // that code page would not open. Convert strictly and open by wide path.
    const int wide_length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path_.c_str(),
        static_cast<int>(path_.size()), nullptr, 0);
    if (wide_length <= 0) {
        return Result<FirmwareImage>::failure(Error::INVALID_ARGUMENT);
    }
    std::wstring wide_path(static_cast<std::size_t>(wide_length), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_.c_str(),
                              static_cast<int>(path_.size()), wide_path.data(),
                              wide_length) != wide_length) {
        return Result<FirmwareImage>::failure(Error::INVALID_ARGUMENT);
    }
    std::FILE* const opened_file = ::_wfopen(wide_path.c_str(), L"rb");
#else
    std::FILE* const opened_file = std::fopen(path_.c_str(), "rb");
#endif
    const auto close_file = [](std::FILE* stream) noexcept { std::fclose(stream); };
    const std::unique_ptr<std::FILE, decltype(close_file)> file(opened_file, close_file);
    if (!file) {
        return Result<FirmwareImage>::failure(Error::NOT_FOUND);
    }

    const auto* expected_manifest = model_.has_value()
        ? loader_firmware_manifest(*model_) : &common_loader_firmware_manifest();
    if (model_.has_value() && expected_manifest == nullptr) {
        return Result<FirmwareImage>::failure(Error::FIRMWARE_REJECTED);
    }

    const std::size_t maximum_size = expected_manifest != nullptr
        ? expected_manifest->size : kLoaderFirmwareBlobSize;
    std::vector<std::uint8_t> data;
    data.reserve(maximum_size);
    Sha256 sha256;
    std::array<std::uint8_t, 512U> chunk{};
    while (!std::feof(file.get())) {
        const std::size_t count = std::fread(chunk.data(), 1U, chunk.size(), file.get());
        // libc++ filebuf can turn a failed read (for example EISDIR) into EOF.
        // Check even a partial read before size/hash validation hides its error.
        if (std::ferror(file.get())) {
            return Result<FirmwareImage>::failure(Error::INTERNAL);
        }
        if (count > 0U) {
            if (data.size() > maximum_size || count > maximum_size - data.size()) {
                return Result<FirmwareImage>::failure(Error::FIRMWARE_REJECTED);
            }
            sha256.update(chunk.data(), count);
            data.insert(data.end(), chunk.data(), chunk.data() + count);
        }
    }
    const auto digest = sha256.finish();
    const auto* manifest = matching_manifest(model_, data.size(), digest);
    if (manifest == nullptr) {
        return Result<FirmwareImage>::failure(Error::FIRMWARE_REJECTED);
    }
    return Result<FirmwareImage>::success(FirmwareImage(std::move(data), manifest));
}

}  // namespace asicen
