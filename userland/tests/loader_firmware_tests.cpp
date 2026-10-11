#include "asicen/firmware.h"
#include "asicen/loader_firmware.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,         \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

bool fingerprint(const std::uint8_t* data, std::size_t size, std::string& output)
{
    std::array<std::uint8_t, 32> digest{};
    CHECK(asicen::loader_firmware_fingerprint(data, size, &digest));
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        result << std::setw(2) << static_cast<unsigned>(byte);
    }
    output = result.str();
    return true;
}

bool fingerprint_vectors()
{
    std::string digest;
    CHECK(fingerprint(nullptr, 0, digest));
    CHECK(digest ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::array<std::uint8_t, 3> abc{{'a', 'b', 'c'}};
    CHECK(fingerprint(abc.data(), abc.size(), digest));
    CHECK(digest ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    struct Vector {
        std::size_t size;
        const char* digest;
    };
    const Vector vectors[] = {
        {55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
        {56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
        {63, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
        {64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
        {65, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
        {16384, "a1f259d4365ed4320c377ce26f5c8c56dcdc9a89e7b641bfd8eabfbbeac86654"},
    };
    for (const auto& vector : vectors) {
        std::vector<std::uint8_t> data(vector.size);
        for (std::size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<std::uint8_t>(i);
        }
        CHECK(fingerprint(data.data(), data.size(), digest));
        CHECK(digest == vector.digest);
    }
    std::array<std::uint8_t, 32> invalid{};
    invalid.fill(0xff);
    CHECK(!asicen::loader_firmware_fingerprint(nullptr, 1, &invalid));
    CHECK((invalid == std::array<std::uint8_t, 32>{}));
    CHECK(!asicen::loader_firmware_fingerprint(nullptr, 0, nullptr));
    return true;
}

bool manifest_policy()
{
    const auto& manifest = asicen::loader_firmware_manifest();
    CHECK(manifest.size == 16384 && manifest.start_address == 0x5399);
    const std::array<asicen::ModelId, 5> supported{{asicen::ModelId::S3u, asicen::ModelId::S3u2,
                                                 asicen::ModelId::W3u2, asicen::ModelId::W3u3,
                                                 asicen::ModelId::W3u3V2}};
    std::vector<std::uint8_t> fake(manifest.size, 0);
    for (const auto model : supported) {
        CHECK(asicen::loader_firmware_supports_model(model));
        CHECK(asicen::validate_loader_firmware(model, fake.data(), fake.size()) ==
              asicen::LoaderFirmwareRead::FingerprintMismatch);
        CHECK(asicen::validate_loader_firmware(model, nullptr, manifest.size) ==
              asicen::LoaderFirmwareRead::FingerprintMismatch);
        CHECK(asicen::validate_loader_firmware(model, fake.data(), fake.size() - 1U) ==
              asicen::LoaderFirmwareRead::WrongSize);
    }
    const auto* windows = asicen::loader_firmware_manifest(asicen::ModelId::W3u3V2);
    CHECK(windows != nullptr && windows->size == manifest.size &&
          windows->start_address == 0x55d6 && windows->sha256 != manifest.sha256);
    for (const auto model : supported) {
        const auto plan = asicen::build_loader_transfer_plan(model);
        CHECK(plan.size() == 20);
        for (const auto& transfer : plan) {
            CHECK(transfer.index == (model == asicen::ModelId::W3u3V2 ? 0x55d6 : 0x5399));
            CHECK(transfer.length == 512 && transfer.blob_offset + transfer.length <= manifest.size);
        }
        CHECK(!plan.empty() && plan.back().request == 0xac);
    }
    for (const auto model : {static_cast<asicen::ModelId>(255)}) {
        CHECK(!asicen::loader_firmware_supports_model(model));
        CHECK(asicen::loader_firmware_manifest(model) == nullptr);
        CHECK(asicen::build_loader_transfer_plan(model).empty());
        CHECK(asicen::validate_loader_firmware(model, nullptr, 0) ==
              asicen::LoaderFirmwareRead::UnsupportedModel);
    }
    return true;
}

bool external_firmware_images(int argc, char** argv)
{
    // Generic file/path/error boundaries are covered by firmware_provider_tests.
    // Optional positive verification reads bytes only. Firmware is not executed
    // or uploaded to a device by this test.
    if (argc >= 2) {
        for (const auto model : {asicen::ModelId::S3u, asicen::ModelId::S3u2, asicen::ModelId::W3u2,
                                 asicen::ModelId::W3u3}) {
            const auto loaded = asicen::FirmwareProvider(argv[1], model).load();
            CHECK(loaded.has_value());
            const auto& image = loaded.value();
            std::vector<std::uint8_t> blob(image.data(), image.data() + image.size());
            CHECK(asicen::validate_loader_firmware(asicen::ModelId::W3u3V2, blob.data(),
                                                  blob.size()) ==
                  asicen::LoaderFirmwareRead::FingerprintMismatch);
            // Changes in skipped transfer regions must also be rejected.
            blob[0x1000] ^= 1U;
            CHECK(asicen::validate_loader_firmware(model, blob.data(), blob.size()) ==
                  asicen::LoaderFirmwareRead::FingerprintMismatch);
        }
    }
    if (argc >= 3) {
        const auto loaded = asicen::FirmwareProvider(argv[2], asicen::ModelId::W3u3V2).load();
        CHECK(loaded.has_value());
        const auto& image = loaded.value();
        CHECK(asicen::validate_loader_firmware(asicen::ModelId::W3u3, image.data(), image.size()) ==
              asicen::LoaderFirmwareRead::FingerprintMismatch);
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (!fingerprint_vectors() || !manifest_policy() || !external_firmware_images(argc, argv)) {
        return 1;
    }
    return 0;
}
