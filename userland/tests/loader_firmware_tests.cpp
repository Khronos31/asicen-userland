#include "asicen/loader_firmware.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool value, const char* name) {
    if (!value) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

void write_file(const std::string& path,
                std::size_t size,
                std::uint8_t first,
                std::uint8_t last) {
    std::vector<char> bytes(size, 0);
    if (size != 0) {
        bytes.front() = static_cast<char>(first);
        bytes.back() = static_cast<char>(last);
    }
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string fingerprint(const std::uint8_t* data, std::size_t size) {
    std::array<std::uint8_t, 32> digest{};
    check(asicen::loader_firmware_fingerprint(data, size, &digest), "compute fingerprint");
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (const auto byte : digest) result << std::setw(2) << static_cast<unsigned>(byte);
    return result.str();
}

void fingerprint_vectors() {
    check(fingerprint(nullptr, 0) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 empty input");
    const std::array<std::uint8_t, 3> abc{{'a', 'b', 'c'}};
    check(fingerprint(abc.data(), abc.size()) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 abc input");
    struct Vector { std::size_t size; const char* digest; };
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
        for (std::size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<std::uint8_t>(i);
        check(fingerprint(data.data(), data.size()) == vector.digest,
              "SHA-256 padding/block boundary vector");
    }
    std::array<std::uint8_t, 32> invalid{};
    invalid.fill(0xff);
    check(!asicen::loader_firmware_fingerprint(nullptr, 1, &invalid),
          "fingerprint rejects null nonempty input");
    check(invalid == std::array<std::uint8_t, 32>{}, "invalid fingerprint clears digest");
    check(!asicen::loader_firmware_fingerprint(nullptr, 0, nullptr),
          "fingerprint rejects null output");
}

void manifest_policy() {
    const auto& manifest = asicen::loader_firmware_manifest();
    check(manifest.size == 16384 && manifest.start_address == 0x5399,
          "manifest has recovered firmware shape");
    const std::array<asicen::ModelId, 5> supported{{asicen::ModelId::S3u,
        asicen::ModelId::S3u2, asicen::ModelId::W3u2, asicen::ModelId::W3u3,
        asicen::ModelId::W3u3V2}};
    std::vector<std::uint8_t> fake(manifest.size, 0);
    for (const auto model : supported) {
        check(asicen::loader_firmware_supports_model(model), "known model has firmware evidence");
        check(asicen::validate_loader_firmware(model, fake.data(), fake.size()) ==
                  asicen::LoaderFirmwareRead::FingerprintMismatch,
              "same-sized unknown firmware fails fingerprint");
        check(asicen::validate_loader_firmware(model, nullptr, manifest.size) ==
                  asicen::LoaderFirmwareRead::FingerprintMismatch,
              "null firmware fails closed");
        check(asicen::validate_loader_firmware(model, fake.data(), fake.size() - 1U) ==
                  asicen::LoaderFirmwareRead::WrongSize,
              "wrong size fails validation");
    }
    const auto* windows = asicen::loader_firmware_manifest(asicen::ModelId::W3u3V2);
    check(windows != nullptr && windows->size == manifest.size &&
              windows->start_address == 0x55d6 && windows->sha256 != manifest.sha256,
          "V2 has a distinct Windows image and command index");
    for (const auto model : supported) {
        const auto plan = asicen::build_loader_transfer_plan(model);
        check(plan.size() == 20, "each verified image has twenty transfers");
        for (const auto& transfer : plan) {
            check(transfer.index == (model == asicen::ModelId::W3u3V2 ? 0x55d6 : 0x5399),
                  "transfer plan uses the selected image command index");
            check(transfer.length == 512 &&
                      transfer.blob_offset + transfer.length <= manifest.size,
                  "transfer is bounded to the fingerprinted image");
        }
        check(!plan.empty() && plan.back().request == 0xac,
              "last transfer commits the selected image");
    }
    for (const auto model : {static_cast<asicen::ModelId>(255)}) {
        check(!asicen::loader_firmware_supports_model(model), "unknown model has no firmware evidence");
        check(asicen::loader_firmware_manifest(model) == nullptr,
              "unknown model has no manifest");
        check(asicen::build_loader_transfer_plan(model).empty(),
              "unknown model has no transfer plan");
        check(asicen::validate_loader_firmware(model, nullptr, 0) ==
                  asicen::LoaderFirmwareRead::UnsupportedModel,
              "unsupported model is rejected before reading bytes");
    }
}

}  // namespace

int main(int argc, char** argv) {
    fingerprint_vectors();
    manifest_policy();
    const std::string short_path = "asicen-loader-test-short.bin";
    write_file(short_path, asicen::kLoaderFirmwareBlobSize - 1, 0x5a, 0xa5);

    std::vector<std::uint8_t> blob;
    check(asicen::read_loader_firmware_file(short_path, &blob) ==
              asicen::LoaderFirmwareRead::WrongSize,
          "reject short firmware");
    check(blob.empty(), "short firmware leaves no data");

    const std::string exact_path = "asicen-loader-test-exact.bin";
    write_file(exact_path, asicen::kLoaderFirmwareBlobSize, 0x5a, 0xa5);
    check(asicen::read_loader_firmware_file(exact_path, &blob) ==
              asicen::LoaderFirmwareRead::Ok,
          "accept exact firmware");
    check(blob.size() == asicen::kLoaderFirmwareBlobSize, "exact firmware size");
    check(blob.front() == 0x5a && blob.back() == 0xa5, "exact firmware payload");
    check(asicen::read_verified_loader_firmware_file(exact_path, asicen::ModelId::S3u2,
                                                   &blob) ==
              asicen::LoaderFirmwareRead::FingerprintMismatch,
          "verified reader rejects same-sized unknown payload");
    check(blob.empty(), "fingerprint rejection leaves no upload data");
    blob.assign(1, 0x5a);
    check(asicen::read_verified_loader_firmware_file(exact_path, static_cast<asicen::ModelId>(255),
                                                   &blob) ==
              asicen::LoaderFirmwareRead::UnsupportedModel,
          "verified reader rejects unknown models before opening");
    check(blob.empty(), "unsupported model leaves no upload data");

    const std::string long_path = "asicen-loader-test-long.bin";
    write_file(long_path, asicen::kLoaderFirmwareBlobSize + 1, 0x5a, 0xa5);
    check(asicen::read_loader_firmware_file(long_path, &blob) ==
              asicen::LoaderFirmwareRead::WrongSize,
          "reject oversized firmware");
    check(blob.empty(), "oversized firmware leaves no data");

    const std::string missing_path = "asicen-loader-test-missing.bin";
    std::remove(missing_path.c_str());
    check(asicen::read_loader_firmware_file(missing_path, &blob) ==
              asicen::LoaderFirmwareRead::OpenFailed,
          "report missing firmware");

    std::remove(short_path.c_str());
    std::remove(exact_path.c_str());
    std::remove(long_path.c_str());

    // Optional local-only positive verification; firmware is never a repository
    // test fixture and is not executed or uploaded by this test.
    if (argc >= 2) {
        for (const auto model : {asicen::ModelId::S3u, asicen::ModelId::S3u2,
                                 asicen::ModelId::W3u2, asicen::ModelId::W3u3}) {
            check(asicen::read_verified_loader_firmware_file(argv[1], model, &blob) ==
                      asicen::LoaderFirmwareRead::Ok,
                  "external known firmware matches manifest");
            if (!blob.empty()) {
                check(asicen::validate_loader_firmware(asicen::ModelId::W3u3V2,
                                                       blob.data(), blob.size()) ==
                          asicen::LoaderFirmwareRead::FingerprintMismatch,
                      "Linux image cannot be selected for V2");
                // Changes in skipped transfer regions must also be rejected.
                blob[0x1000] ^= 1U;
                check(asicen::validate_loader_firmware(model, blob.data(), blob.size()) ==
                          asicen::LoaderFirmwareRead::FingerprintMismatch,
                      "one-byte firmware change fails fingerprint");
            }
        }
    }
    if (argc >= 3) {
        check(asicen::read_verified_loader_firmware_file(argv[2], asicen::ModelId::W3u3V2,
                                                       &blob) ==
                  asicen::LoaderFirmwareRead::Ok,
              "external Windows image matches V2 manifest");
        if (!blob.empty()) {
            check(asicen::validate_loader_firmware(asicen::ModelId::W3u3,
                                                   blob.data(), blob.size()) ==
                      asicen::LoaderFirmwareRead::FingerprintMismatch,
                  "Windows image is not silently substituted in the legacy manifest");
        }
    }

    return failures == 0 ? 0 : 1;
}
