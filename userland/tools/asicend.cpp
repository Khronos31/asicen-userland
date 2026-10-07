#include "asicen/backend.h"
#include "asicen/ipc.h"
#include "asicen/receiver_service.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;

void on_signal(int) {
    g_stop = 1;
}

bool read_exact(int fd, std::uint8_t* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t rc = ::read(fd, data + done, size - done);
        if (rc == 0) {
            return false;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        done += static_cast<std::size_t>(rc);
    }
    return true;
}

bool write_all(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t rc = ::write(fd, data + done, size - done);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        done += static_cast<std::size_t>(rc);
    }
    return true;
}

void send_status(int fd,
                 asicen::IpcStatus status,
                 std::uint64_t lease_id,
                 std::size_t receiver_count) {
    const auto response = asicen::encode_response(
        asicen::IpcResponse{status, lease_id,
                            static_cast<std::uint32_t>(receiver_count)});
    write_all(fd, response.data(), response.size());
}

void handle_client(int fd,
                   asicen::ReceiverLeaseTable* leases,
                   asicen::DeviceBackend* backend) {
    std::array<std::uint8_t, asicen::kIpcMessageSize> raw{};
    if (!read_exact(fd, raw.data(), raw.size())) {
        ::close(fd);
        return;
    }

    asicen::IpcRequest request{};
    if (!asicen::decode_request(raw.data(), raw.size(), &request)) {
        send_status(fd, asicen::IpcStatus::Invalid, 0, leases->size());
        ::close(fd);
        return;
    }

    if (request.command == asicen::IpcCommand::Status) {
        send_status(fd, asicen::IpcStatus::Ok, 0, leases->size());
        ::close(fd);
        return;
    }

    if (request.receiver >= leases->size() || request.packet_count == 0 ||
        request.packet_count > 1000000U) {
        send_status(fd, asicen::IpcStatus::Invalid, 0, leases->size());
        ::close(fd);
        return;
    }

    const auto lease = leases->acquire(request.receiver);
    if (!lease.has_value()) {
        send_status(fd, asicen::IpcStatus::Busy, 0, leases->size());
        ::close(fd);
        return;
    }

    auto stream = backend->open_stream(request.receiver);
    if (!stream) {
        send_status(fd, asicen::IpcStatus::Internal, *lease, leases->size());
        leases->release(request.receiver, *lease);
        ::close(fd);
        return;
    }

    const auto response = asicen::encode_response(
        asicen::IpcResponse{asicen::IpcStatus::Ok, *lease,
                            static_cast<std::uint32_t>(leases->size())});
    if (!write_all(fd, response.data(), response.size())) {
        leases->release(request.receiver, *lease);
        ::close(fd);
        return;
    }

    leases->set_streaming(request.receiver, *lease, true);

    constexpr std::size_t kPacketBytes = 188;
    constexpr std::size_t kChunkPackets = 256;
    std::array<std::uint8_t, kPacketBytes * kChunkPackets> buffer{};
    std::uint64_t remaining =
        static_cast<std::uint64_t>(request.packet_count) * kPacketBytes;
    bool ok = true;

    while (remaining != 0) {
        const std::size_t capacity =
            std::min<std::uint64_t>(remaining, buffer.size());
        std::size_t bytes_read = 0;
        if (!stream->read(buffer.data(), capacity, &bytes_read) ||
            bytes_read == 0 || bytes_read > capacity ||
            (bytes_read % kPacketBytes) != 0) {
            ok = false;
            break;
        }
        if (!write_all(fd, buffer.data(), bytes_read)) {
            ok = false;
            break;
        }
        remaining -= bytes_read;
    }

    if (!ok) {
        leases->set_error(request.receiver, *lease);
    }
    leases->release(request.receiver, *lease);
    ::close(fd);
}

bool parse_args(int argc, char** argv, std::string* socket_path) {
    bool mock = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mock") {
            mock = true;
        } else if (arg == "--socket" && i + 1 < argc) {
            *socket_path = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "usage: asicend --mock --socket PATH\n";
            std::exit(0);
        } else {
            return false;
        }
    }
    return mock && !socket_path->empty();
}

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path;
    if (!parse_args(argc, argv, &socket_path)) {
        std::cerr << "usage: asicend --mock --socket PATH\n";
        return 2;
    }
    if (socket_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        std::cerr << "socket path too long\n";
        return 2;
    }

    auto backend = asicen::make_mock_backend(4);
    if (!backend) {
        std::cerr << "backend creation failed\n";
        return 1;
    }
    asicen::ReceiverLeaseTable leases(backend->receiver_count());

    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT, on_signal);
    ::signal(SIGTERM, on_signal);

    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        std::perror("socket");
        return 1;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    ::unlink(socket_path.c_str());

    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::perror("bind");
        ::close(listener);
        return 1;
    }
    ::chmod(socket_path.c_str(), 0600);
    if (::listen(listener, 16) != 0) {
        std::perror("listen");
        ::unlink(socket_path.c_str());
        ::close(listener);
        return 1;
    }

    std::cerr << "asicend mock ready socket=" << socket_path
              << " receivers=" << backend->receiver_count() << '\n';

    while (!g_stop) {
        pollfd pfd{listener, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 200);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0 || (pfd.revents & POLLIN) == 0) {
            continue;
        }

        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        std::thread(handle_client, client, &leases, backend.get()).detach();
    }

    ::close(listener);
    ::unlink(socket_path.c_str());
    return 0;
}
