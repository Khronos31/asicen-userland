#include "asicen/loader_firmware.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
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

}  // namespace

int main() {
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

    return failures == 0 ? 0 : 1;
}
