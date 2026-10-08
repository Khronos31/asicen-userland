// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/transport_capture.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
struct FileIdentity { dev_t device = 0; ino_t inode = 0; };

struct SeedWiper {
    std::array<std::uint8_t, 16>* value;
    ~SeedWiper() { if (value != nullptr) value->fill(0); }
};

bool read_seed(const std::string& path, std::array<std::uint8_t, 16>* seed,
               FileIdentity* identity) {
    if (seed == nullptr || path.empty() || path.front() != '/') return false;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat info {};
    bool ok = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
              info.st_uid == ::geteuid() && (info.st_mode & 0777) == 0600 &&
              info.st_size == 16;
    if (ok && identity != nullptr) {
        identity->device = info.st_dev;
        identity->inode = info.st_ino;
    }
    std::size_t offset = 0;
    while (ok && offset < seed->size()) {
        const ssize_t count = ::read(fd, seed->data() + offset, seed->size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        offset += static_cast<std::size_t>(count);
    }
    std::uint8_t extra = 0;
    if (ok) {
        ssize_t count = -1;
        do { count = ::read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
        ok = count == 0;
    }
    ::close(fd);
    if (!ok) seed->fill(0);
    return ok;
}

bool write_all(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = ::write(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    std::string seed_path;
    std::string input_path;
    std::string output_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--seed-file" && i + 1 < argc) seed_path = argv[++i];
        else if (arg == "--input" && i + 1 < argc) input_path = argv[++i];
        else if (arg == "--output" && i + 1 < argc) output_path = argv[++i];
        else {
            std::cerr << "usage: " << argv[0]
                      << " --seed-file /private/seed --input raw.bin --output ts.bin\n";
            return 2;
        }
    }
    if (seed_path.empty() || input_path.empty() || output_path.empty() ||
        input_path == "-" || output_path == "-") {
        std::cerr << "seed, input and output file paths are required\n";
        return 2;
    }

    std::array<std::uint8_t, 16> seed{};
    const SeedWiper seed_wiper{&seed};
    FileIdentity seed_identity{};
    if (!read_seed(seed_path, &seed, &seed_identity)) {
        std::cerr << "seed must be an absolute owner-private mode-0600 regular file of exactly 16 bytes\n";
        return 2;
    }
    const int input = ::open(input_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0) {
        std::cerr << "cannot open raw input: " << std::strerror(errno) << '\n';
        return 1;
    }
    struct stat input_stat {};
    if (::fstat(input, &input_stat) != 0 || !S_ISREG(input_stat.st_mode)) {
        std::cerr << "raw input must be a regular file\n";
        ::close(input);
        return 1;
    }
    const int output = ::open(output_path.c_str(),
                              O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
        std::cerr << "cannot open TS output: " << std::strerror(errno) << '\n';
        ::close(input);
        return 1;
    }
    struct stat output_stat {};
    bool ok = ::fstat(output, &output_stat) == 0 && S_ISREG(output_stat.st_mode) &&
              !(input_stat.st_dev == output_stat.st_dev &&
                input_stat.st_ino == output_stat.st_ino) &&
              !(seed_identity.device == output_stat.st_dev &&
                seed_identity.inode == output_stat.st_ino) &&
              ::fchmod(output, 0600) == 0 &&
              ::ftruncate(output, 0) == 0;
    if (!ok) {
        std::cerr << "output must be a distinct regular file; no conversion performed\n";
        ::close(input);
        ::close(output);
        return 1;
    }

    asicen::TransportCaptureDecoderV7 decoder(seed.data(), seed.size());
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t raw_bytes = 0;
    std::uint64_t packet_bytes = 0;
    while (ok) {
        const ssize_t count = ::read(input, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { ok = false; break; }
        if (count == 0) break;
        raw_bytes += static_cast<std::uint64_t>(count);
        auto packets = decoder.push(buffer.data(), static_cast<std::size_t>(count));
        if (!packets.empty()) {
            ok = write_all(output, packets.data(), packets.size());
            packet_bytes += packets.size();
        }
    }
    const bool close_input_ok = ::close(input) == 0;
    const bool close_output_ok = ::close(output) == 0;
    std::cerr << "transform raw_bytes=" << raw_bytes
              << " output_packet_bytes=" << packet_bytes
              << " packets=" << packet_bytes / asicen::kMpegTsPacketSize
              << " discarded_framing_bytes=" << decoder.discarded_bytes()
              << " pending_at_eof=" << decoder.pending_bytes()
              << " result=" << (ok && close_input_ok && close_output_ok ? "ok" : "failed")
              << '\n';
    return ok && close_input_ok && close_output_ok ? 0 : 1;
}
