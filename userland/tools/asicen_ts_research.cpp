// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/ipc.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

bool write_all(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t rc = ::write(fd, data + done, size - done);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<std::size_t>(rc);
    }
    return true;
}

bool read_exact(int fd, std::uint8_t* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t rc = ::read(fd, data + done, size - done);
        if (rc == 0) return false;
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<std::size_t>(rc);
    }
    return true;
}

bool parse_u32(const char* text, std::uint32_t* out) {
    if (text == nullptr || out == nullptr) return false;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == nullptr || *end != '\0' || value > 0xffffffffUL) return false;
    *out = static_cast<std::uint32_t>(value);
    return true;
}

bool parse_args(int argc, char** argv, std::string* socket_path,
                std::uint32_t* receiver, std::uint32_t* packet_count) {
    bool have_receiver = false;
    bool have_count = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            *socket_path = argv[++i];
        } else if (arg == "--receiver" && i + 1 < argc) {
            have_receiver = parse_u32(argv[++i], receiver);
            if (!have_receiver) return false;
        } else if (arg == "--packet-count" && i + 1 < argc) {
            have_count = parse_u32(argv[++i], packet_count);
            if (!have_count) return false;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "usage: asicen-ts --socket PATH --receiver N --packet-count N\n";
            std::exit(0);
        } else {
            return false;
        }
    }
    return !socket_path->empty() && have_receiver && have_count && *packet_count != 0;
}

}  // namespace

int run_asicen_ts_research(int argc, char** argv) {
    std::string socket_path;
    std::uint32_t receiver = 0;
    std::uint32_t packet_count = 0;
    if (!parse_args(argc, argv, &socket_path, &receiver, &packet_count)) {
        std::cerr << "usage: asicen-ts --socket PATH --receiver N --packet-count N\n";
        return 2;
    }
    if (socket_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        std::cerr << "socket path too long\n";
        return 2;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return 1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::perror("connect");
        ::close(fd);
        return 3;
    }

    const auto request = asicen::encode_request(
        asicen::IpcRequest{asicen::IpcCommand::Stream, receiver, packet_count});
    if (!write_all(fd, request.data(), request.size())) {
        std::cerr << "failed to send request\n";
        ::close(fd);
        return 1;
    }

    std::array<std::uint8_t, asicen::kIpcMessageSize> raw{};
    if (!read_exact(fd, raw.data(), raw.size())) {
        std::cerr << "failed to read response\n";
        ::close(fd);
        return 1;
    }
    asicen::IpcResponse response{};
    if (!asicen::decode_response(raw.data(), raw.size(), &response)) {
        std::cerr << "invalid response\n";
        ::close(fd);
        return 6;
    }
    if (response.status == asicen::IpcStatus::Busy) {
        std::cerr << "receiver busy\n";
        ::close(fd);
        return 4;
    }
    if (response.status != asicen::IpcStatus::Ok) {
        std::cerr << "stream rejected\n";
        ::close(fd);
        return 2;
    }

    std::uint64_t remaining = static_cast<std::uint64_t>(packet_count) * 188U;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (remaining != 0) {
        const std::size_t want = remaining < buffer.size()
            ? static_cast<std::size_t>(remaining) : buffer.size();
        const ssize_t rc = ::read(fd, buffer.data(), want);
        if (rc == 0) {
            std::cerr << "short stream\n";
            ::close(fd);
            return 7;
        }
        if (rc < 0) {
            if (errno == EINTR) continue;
            std::perror("read");
            ::close(fd);
            return 7;
        }
        if (std::fwrite(buffer.data(), 1, static_cast<std::size_t>(rc), stdout) !=
            static_cast<std::size_t>(rc)) {
            std::cerr << "stdout write failed\n";
            ::close(fd);
            return 1;
        }
        remaining -= static_cast<std::uint64_t>(rc);
    }

    ::close(fd);
    return 0;
}
