// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/backend.h"
#include "asicen/ipc.h"
#include "asicen/receiver_service.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (done < size) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return false;
        pollfd wait{fd, POLLIN, 0};
        const int ready = ::poll(&wait, 1, static_cast<int>(remaining));
        if (ready <= 0 || (wait.revents & (POLLERR | POLLNVAL)) != 0) return false;
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (done < size) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return false;
        pollfd wait{fd, POLLOUT, 0};
        const int ready = ::poll(&wait, 1, static_cast<int>(remaining));
        if (ready <= 0 || (wait.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return false;
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

struct ClientRegistry {
    std::mutex mutex;
    std::vector<int> sockets;
    void add(int fd) { std::lock_guard<std::mutex> lock(mutex); sockets.push_back(fd); }
    void remove(int fd) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = std::find(sockets.begin(), sockets.end(), fd);
        if (found != sockets.end()) sockets.erase(found);
    }
    void interrupt_all() {
        std::lock_guard<std::mutex> lock(mutex);
        for (const int fd : sockets) (void)::shutdown(fd, SHUT_RDWR);
    }
};

void close_registered_client(int fd, ClientRegistry* registry)
{
    // Remove ownership before close so concurrent shutdown cannot call
    // shutdown() on an fd number that the process has already reused.
    registry->remove(fd);
    ::close(fd);
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
                   asicen::DeviceBackend* backend,
                   ClientRegistry* registry) {
    std::array<std::uint8_t, asicen::kIpcMessageSize> raw{};
    if (!read_exact(fd, raw.data(), raw.size())) {
        close_registered_client(fd, registry);
        return;
    }

    asicen::IpcRequest request{};
    if (!asicen::decode_request(raw.data(), raw.size(), &request)) {
        send_status(fd, asicen::IpcStatus::Invalid, 0, leases->size());
        close_registered_client(fd, registry);
        return;
    }

    if (request.command == asicen::IpcCommand::Status) {
        send_status(fd, asicen::IpcStatus::Ok, 0, leases->size());
        close_registered_client(fd, registry);
        return;
    }

    if (request.receiver >= leases->size() || request.packet_count == 0 ||
        request.packet_count > 1000000U) {
        send_status(fd, asicen::IpcStatus::Invalid, 0, leases->size());
        close_registered_client(fd, registry);
        return;
    }

    const auto lease = leases->acquire(request.receiver);
    if (!lease.has_value()) {
        send_status(fd, asicen::IpcStatus::Busy, 0, leases->size());
        close_registered_client(fd, registry);
        return;
    }

    auto stream = backend->open_stream(request.receiver);
    if (!stream) {
        send_status(fd, asicen::IpcStatus::Internal, *lease, leases->size());
        leases->release(request.receiver, *lease);
        close_registered_client(fd, registry);
        return;
    }

    const auto response = asicen::encode_response(
        asicen::IpcResponse{asicen::IpcStatus::Ok, *lease,
                            static_cast<std::uint32_t>(leases->size())});
    if (!write_all(fd, response.data(), response.size())) {
        leases->release(request.receiver, *lease);
        close_registered_client(fd, registry);
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
    close_registered_client(fd, registry);
}

bool parse_args(int argc, char** argv, std::string* socket_path) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            *socket_path = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "usage: asicend-mock --socket PATH\n";
            std::exit(0);
        } else {
            return false;
        }
    }
    return !socket_path->empty();
}

}  // namespace

int run_asicend_research(int argc, char** argv) {
    std::string socket_path;
    if (!parse_args(argc, argv, &socket_path)) {
        std::cerr << "usage: asicend-mock --socket PATH\n";
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

    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::perror("bind");
        ::close(listener);
        return 1;
    }
    struct stat endpoint_identity{};
    if (::lstat(socket_path.c_str(), &endpoint_identity) != 0 ||
        !S_ISSOCK(endpoint_identity.st_mode)) {
        ::close(listener);
        return 1;
    }
    ::chmod(socket_path.c_str(), 0600);
    if (::listen(listener, 16) != 0) {
        std::perror("listen");
        struct stat current{};
        if (::lstat(socket_path.c_str(), &current) == 0 &&
            current.st_dev == endpoint_identity.st_dev &&
            current.st_ino == endpoint_identity.st_ino) ::unlink(socket_path.c_str());
        ::close(listener);
        return 1;
    }

    std::cerr << "asicend mock ready socket=" << socket_path
              << " receivers=" << backend->receiver_count() << '\n';

    ClientRegistry clients;
    struct ClientThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<ClientThread> client_threads;

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
        for (std::size_t index = client_threads.size(); index > 0U; --index) {
            auto& record = client_threads[index - 1U];
            if (record.finished->load()) {
                record.thread.join();
                client_threads.erase(client_threads.begin() +
                                     static_cast<std::ptrdiff_t>(index - 1U));
            }
        }
        if (client_threads.size() >= 32U) {
            ::close(client);
            continue;
        }
        timeval send_timeout{0, 250000};
        (void)::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                           &send_timeout, sizeof(send_timeout));
        clients.add(client);
        auto finished = std::make_shared<std::atomic<bool>>(false);
        client_threads.push_back(ClientThread{
            std::thread([client, &leases, backend_ptr = backend.get(), &clients, finished] {
                handle_client(client, &leases, backend_ptr, &clients);
                finished->store(true);
            }), finished});
    }

    ::close(listener);
    clients.interrupt_all();
    for (auto& record : client_threads)
        if (record.thread.joinable()) record.thread.join();
    struct stat current{};
    if (::lstat(socket_path.c_str(), &current) == 0 &&
        current.st_dev == endpoint_identity.st_dev &&
        current.st_ino == endpoint_identity.st_ino) ::unlink(socket_path.c_str());
    return 0;
}
