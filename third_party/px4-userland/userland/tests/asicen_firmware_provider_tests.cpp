// SPDX-License-Identifier: GPL-2.0-only
// ASICEN equivalent of the pinned FirmwareProvider policy/error boundaries.
#include "asicen/firmware.h"
#include "asicen/loader_firmware.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace asicen;
using px4::userland::Error;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n",             \
                         __FILE__, __LINE__, #condition);                    \
            return false;                                                   \
        }                                                                   \
    } while (false)

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::error_code error;
        const auto base = std::filesystem::temp_directory_path(error);
        if (error) {
            return;
        }
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto candidate = base / ("asicen-firmware-" + std::to_string(stamp) +
                                           "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error) {
                return;
            }
        }
    }
    ~TemporaryDirectory() noexcept
    {
        std::error_code error;
        if (!path_.empty()) {
            std::filesystem::remove_all(path_, error);
        }
    }
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

bool check_rejected(const FirmwareProvider& provider, Error error)
{
    const auto loaded = provider.load();
    CHECK(!loaded && loaded.error() == error);
    CHECK(loaded.value().size() == 0U);
    CHECK(loaded.value().bytes().size == 0U);
    return true;
}

bool test_provider_errors_and_policy(const char* known_image)
{
    TemporaryDirectory runtime;
    CHECK(!runtime.path().empty());
    CHECK(check_rejected(FirmwareProvider("", ModelId::W3u3), Error::INVALID_ARGUMENT));
    CHECK(check_rejected(FirmwareProvider(std::string("file\0suffix", 11U), ModelId::W3u3),
                         Error::INVALID_ARGUMENT));
#if defined(_WIN32)
    CHECK(check_rejected(FirmwareProvider(std::string("\xc0\xaf", 2U), ModelId::W3u3),
                         Error::INVALID_ARGUMENT));
#else
    CHECK(check_rejected(FirmwareProvider(runtime.path().u8string(), ModelId::W3u3),
                         Error::INTERNAL));
    const auto directory_link = runtime.path() / "directory-link";
    std::error_code link_error;
    std::filesystem::create_directory_symlink(runtime.path(), directory_link, link_error);
    CHECK(!link_error);
    CHECK(check_rejected(FirmwareProvider(directory_link.u8string(), ModelId::W3u3),
                         Error::INTERNAL));
#endif
    const std::string missing = (runtime.path() / "missing").u8string();
    CHECK(check_rejected(FirmwareProvider(missing, ModelId::W3u3), Error::NOT_FOUND));
    const std::string image = (runtime.path() / std::filesystem::u8path("firmware-\xc4\x80.bin")).u8string();
    for (const std::size_t size : {0U, 1U, 511U, 512U, 513U, 16383U, 16384U, 16385U, 16896U}) {
        std::ofstream file(std::filesystem::u8path(image), std::ios::out | std::ios::binary);
        const std::vector<std::uint8_t> bytes(size, 0U);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.close();
        CHECK(file.good());
        const FirmwareProvider provider(image, ModelId::W3u3);
        CHECK(provider.path() == image);
        CHECK(check_rejected(provider, Error::FIRMWARE_REJECTED));
    }
    CHECK(check_rejected(FirmwareProvider(image + std::string("\0suffix", 7U), ModelId::W3u3),
                         Error::INVALID_ARGUMENT));
    CHECK(check_rejected(FirmwareProvider(image, static_cast<ModelId>(255)),
                         Error::FIRMWARE_REJECTED));
    if (known_image != nullptr) {
        for (const ModelId model : {ModelId::S3u, ModelId::S3u2, ModelId::W3u2, ModelId::W3u3}) {
            const auto loaded = FirmwareProvider(known_image, model).load();
            CHECK(loaded && loaded.value().size() == kLoaderFirmwareBlobSize);
            CHECK(loaded.value().bytes().data == loaded.value().data());
            std::array<std::uint8_t, 32U> fingerprint{};
            CHECK(loader_firmware_fingerprint(loaded.value().data(), loaded.value().size(),
                                               &fingerprint));
            CHECK(fingerprint == loader_firmware_manifest(model)->sha256);
        }
        CHECK(check_rejected(FirmwareProvider(known_image, ModelId::W3u3V2),
                             Error::FIRMWARE_REJECTED));
        CHECK(check_rejected(FirmwareProvider(known_image, static_cast<ModelId>(255)),
                             Error::FIRMWARE_REJECTED));
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    if (!test_provider_errors_and_policy(argc > 1 ? argv[1] : nullptr)) {
        std::fprintf(stderr, "FAIL firmware_provider\n");
        return 1;
    }
    std::printf("PASS firmware_provider\n");
    return 0;
}
