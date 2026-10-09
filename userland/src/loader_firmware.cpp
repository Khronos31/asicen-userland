#include "asicen/loader_firmware.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <utility>

namespace asicen {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants{{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
}};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned count) noexcept {
    return (value >> count) | (value << (32U - count));
}

void sha256_block(const std::uint8_t* block,
                  std::array<std::uint32_t, 8>* state) noexcept {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) {
        for (std::size_t j = 0; j < 4; ++j)
            words[i] = (words[i] << 8U) | block[i * 4 + j];
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
        const std::uint32_t x = words[i - 15];
        const std::uint32_t y = words[i - 2];
        const std::uint32_t s0 = rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3U);
        const std::uint32_t s1 = rotate_right(y, 17) ^ rotate_right(y, 19) ^ (y >> 10U);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    auto work = *state;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::uint32_t s1 = rotate_right(work[4], 6) ^
                                 rotate_right(work[4], 11) ^ rotate_right(work[4], 25);
        const std::uint32_t choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
        const std::uint32_t t1 = work[7] + s1 + choose + kSha256RoundConstants[i] + words[i];
        const std::uint32_t s0 = rotate_right(work[0], 2) ^
                                 rotate_right(work[0], 13) ^ rotate_right(work[0], 22);
        const std::uint32_t majority = (work[0] & work[1]) ^
                                       (work[0] & work[2]) ^ (work[1] & work[2]);
        for (std::size_t j = 7; j > 0; --j) work[j] = work[j - 1];
        work[4] += t1;
        work[0] = t1 + s0 + majority;
    }
    for (std::size_t i = 0; i < state->size(); ++i) (*state)[i] += work[i];
}

}  // namespace

const LoaderFirmwareManifest& loader_firmware_manifest() noexcept {
    // Static provenance: the single FirmBin symbol in the 2013 PLEX Linux
    // loader.ko is identical in both architecture packages. No blob is embedded.
    static constexpr LoaderFirmwareManifest manifest{
        "PLEX Linux 130109 shared loader", kLoaderFirmwareBlobSize,
        kLoaderFirmwareStartAddress,
        {{0xb4, 0x5d, 0x51, 0x02, 0x00, 0xa1, 0x69, 0x0b,
          0x3c, 0xa3, 0x58, 0xd9, 0x3d, 0xe1, 0x3f, 0x40,
          0xe1, 0xd3, 0x56, 0x7b, 0x66, 0x3c, 0x17, 0xe7,
          0x73, 0x34, 0x9a, 0xd9, 0x6f, 0x59, 0x7a, 0xa8}}};
    return manifest;
}

const LoaderFirmwareManifest* loader_firmware_manifest(ModelId model) noexcept {
    // The identical x64 Windows loaders in the W3U2, W3U3 and W3U3 V2
    // packages decode a different static image. V2 is assigned that image
    // only; its firmware compatibility does not imply frontend support.
    static constexpr LoaderFirmwareManifest windows{
        "PLEX Windows shared loader", kLoaderFirmwareBlobSize, 0x55d6,
        {{0x5d, 0xb3, 0x9f, 0xac, 0xda, 0x14, 0x0b, 0xdc,
          0xb3, 0xa2, 0x38, 0xda, 0xd1, 0x0d, 0xa5, 0xaa,
          0xc7, 0x52, 0xa1, 0xba, 0x3a, 0xaf, 0x17, 0xe2,
          0x23, 0x9f, 0x8d, 0x79, 0xa0, 0xad, 0x6a, 0x2a}}};
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

bool loader_firmware_supports_model(ModelId model) noexcept {
    return loader_firmware_manifest(model) != nullptr;
}

bool loader_firmware_fingerprint(const std::uint8_t* data, std::size_t length,
                                std::array<std::uint8_t, 32>* out) noexcept {
    if (out == nullptr) return false;
    out->fill(0);
    if ((data == nullptr && length != 0) ||
        static_cast<std::uint64_t>(length) >
            std::numeric_limits<std::uint64_t>::max() / 8U)
        return false;
    std::array<std::uint32_t, 8> state{{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U}};
    const std::size_t remaining = length % 64U;
    const std::size_t full_length = length - remaining;
    for (std::size_t offset = 0; offset < full_length; offset += 64U)
        sha256_block(data + offset, &state);

    std::array<std::uint8_t, 128> tail{};
    if (remaining != 0) std::copy_n(data + full_length, remaining, tail.begin());
    tail[remaining] = 0x80U;
    const std::size_t padded = remaining < 56U ? 64U : 128U;
    const std::uint64_t bits = static_cast<std::uint64_t>(length) * 8U;
    for (unsigned i = 0; i < 8; ++i)
        tail[padded - 1U - i] = static_cast<std::uint8_t>(bits >> (i * 8U));
    for (std::size_t offset = 0; offset < padded; offset += 64U)
        sha256_block(tail.data() + offset, &state);
    for (std::size_t i = 0; i < state.size(); ++i) {
        for (unsigned j = 0; j < 4; ++j)
            (*out)[i * 4U + j] = static_cast<std::uint8_t>(state[i] >> ((3U - j) * 8U));
    }
    return true;
}

LoaderFirmwareRead validate_loader_firmware(ModelId model,
                                            const std::uint8_t* data,
                                            std::size_t length) noexcept {
    const auto* manifest = loader_firmware_manifest(model);
    if (manifest == nullptr) return LoaderFirmwareRead::UnsupportedModel;
    if (length != manifest->size) return LoaderFirmwareRead::WrongSize;
    std::array<std::uint8_t, 32> digest{};
    if (!loader_firmware_fingerprint(data, length, &digest) ||
        digest != manifest->sha256)
        return LoaderFirmwareRead::FingerprintMismatch;
    return LoaderFirmwareRead::Ok;
}

LoaderFirmwareRead read_verified_loader_firmware_file(
    const std::string& path, ModelId model, std::vector<std::uint8_t>* out) {
    if (out == nullptr) return LoaderFirmwareRead::OpenFailed;
    out->clear();
    if (!loader_firmware_supports_model(model)) return LoaderFirmwareRead::UnsupportedModel;
    std::vector<std::uint8_t> data;
    const auto read = read_loader_firmware_file(path, &data);
    if (read != LoaderFirmwareRead::Ok) return read;
    const auto validation = validate_loader_firmware(model, data.data(), data.size());
    if (validation != LoaderFirmwareRead::Ok) return validation;
    *out = std::move(data);
    return LoaderFirmwareRead::Ok;
}

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
    return build_loader_transfer_plan(ModelId::W3u3);
}

std::vector<LoaderTransfer> build_loader_transfer_plan(ModelId model) {
    std::vector<LoaderTransfer> result;
    const auto* manifest = loader_firmware_manifest(model);
    if (manifest == nullptr) return result;
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
                manifest->start_address,
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
