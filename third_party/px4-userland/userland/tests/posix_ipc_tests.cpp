// SPDX-License-Identifier: GPL-2.0-only
#include "px4/posix_ipc.h"

#include "posix_ipc_test_access.h"
#include "test_temp_directory.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

constexpr uid_t kTestOwnerUid = static_cast<uid_t>(1001U);
constexpr uid_t kTestOtherUid = static_cast<uid_t>(1002U);
constexpr gid_t kTestEffectiveGid = static_cast<gid_t>(2001U);
constexpr gid_t kTestSupplementaryGid = static_cast<gid_t>(2002U);
constexpr gid_t kTestUnrelatedGid = static_cast<gid_t>(2003U);

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

int matching_supplementary_group(int size, gid_t* groups) noexcept
{
    if (size == 0) {
        return 1;
    }
    if (size < 1 || groups == nullptr) {
        return -1;
    }
    groups[0] = kTestSupplementaryGid;
    return 1;
}

int failed_group_query(int, gid_t*) noexcept
{
    return -1;
}

int over_limit_group_query(int size, gid_t* groups) noexcept
{
    if (size == 0) {
        return 65537;
    }
    (void)groups;
    return -1;
}

bool test_ownership_group_authorization()
{
    CHECK(PosixIpcTestAccess::ownership_allowed(
        kTestOwnerUid, kTestUnrelatedGid, kTestOwnerUid, kTestEffectiveGid,
        false, failed_group_query));

    CHECK(!PosixIpcTestAccess::ownership_allowed(
        kTestOtherUid, kTestEffectiveGid, kTestOwnerUid, kTestEffectiveGid,
        false, matching_supplementary_group));
    CHECK(PosixIpcTestAccess::ownership_allowed(
        kTestOtherUid, kTestEffectiveGid, kTestOwnerUid, kTestEffectiveGid,
        true, failed_group_query));

    CHECK(!PosixIpcTestAccess::ownership_allowed(
        kTestOtherUid, kTestSupplementaryGid, kTestOwnerUid, kTestEffectiveGid,
        false, matching_supplementary_group));
    CHECK(PosixIpcTestAccess::ownership_allowed(
        kTestOtherUid, kTestSupplementaryGid, kTestOwnerUid, kTestEffectiveGid,
        true, matching_supplementary_group));
    CHECK(!PosixIpcTestAccess::ownership_allowed(
        kTestOtherUid, kTestUnrelatedGid, kTestOwnerUid, kTestEffectiveGid,
        true, matching_supplementary_group));

    CHECK(!PosixIpcTestAccess::ownership_allowed(
        kTestOtherUid, kTestSupplementaryGid, kTestOwnerUid, kTestEffectiveGid,
        true, failed_group_query));
    CHECK(!PosixIpcTestAccess::ownership_allowed(
        kTestOtherUid, kTestSupplementaryGid, kTestOwnerUid, kTestEffectiveGid,
        true, over_limit_group_query));
    return true;
}

class TemporaryDirectory final {
public:
    explicit TemporaryDirectory(mode_t mode)
    {
        std::string pattern = test::temporary_directory_template("px4-posix-ipc-");
        char* created = ::mkdtemp(pattern.data());
        if (created != nullptr) {
            path_ = created;
            valid_ = ::chmod(path_.c_str(), mode) == 0;
        }
    }

    ~TemporaryDirectory() noexcept
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    bool valid() const noexcept { return valid_; }
    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    bool valid_ = false;
};

class EnvironmentRestore final {
public:
    explicit EnvironmentRestore(const char* name) : name_(name)
    {
        const char* value = std::getenv(name);
        if (value != nullptr) {
            existed_ = true;
            value_ = value;
        }
    }

    ~EnvironmentRestore() noexcept
    {
        if (existed_) {
            (void)::setenv(name_.c_str(), value_.c_str(), 1);
        } else {
            (void)::unsetenv(name_.c_str());
        }
    }

    EnvironmentRestore(const EnvironmentRestore&) = delete;
    EnvironmentRestore& operator=(const EnvironmentRestore&) = delete;

private:
    std::string name_;
    std::string value_;
    bool existed_ = false;
};

mode_t permissions(const std::string& path)
{
    struct stat status {};
    return ::lstat(path.c_str(), &status) == 0 ? status.st_mode & 07777 : 0;
}

bool is_socket(const std::string& path)
{
    struct stat status {};
    return ::lstat(path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode);
}

std::uint64_t inode_of(const std::string& path)
{
    struct stat status {};
    return ::lstat(path.c_str(), &status) == 0 ?
               static_cast<std::uint64_t>(status.st_ino) : 0U;
}

std::vector<std::uint8_t> make_empty_frame(MessageType type,
                                           std::uint32_t request_id)
{
    std::vector<std::uint8_t> frame(kFrameHeaderSize, 0U);
    const FrameHeader header{kProtocolMajor, kProtocolMinor, type,
                             MessageKind::request, request_id, 0U};
    const auto encoded = encode_frame(header, ByteView{nullptr, 0U},
                                      MutableByteView{frame.data(), frame.size()});
    return encoded ? frame : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t> make_hello_frame(std::uint32_t request_id)
{
    std::array<std::uint8_t, 12U> payload{};
    const auto payload_size = encode_payload(
        HelloRequestPayload{1U, 0U, 1U, 0U, 0U},
        MutableByteView{payload.data(), payload.size()});
    if (!payload_size) {
        return {};
    }
    std::vector<std::uint8_t> frame(kFrameHeaderSize + payload_size.value(), 0U);
    const FrameHeader header{kProtocolMajor, kProtocolMinor, MessageType::HELLO,
                             MessageKind::request, request_id,
                             static_cast<std::uint32_t>(payload_size.value())};
    const auto encoded = encode_frame(
        header, ByteView{payload.data(), payload_size.value()},
        MutableByteView{frame.data(), frame.size()});
    return encoded ? frame : std::vector<std::uint8_t>{};
}

bool raw_send_all(int fd, const std::uint8_t* data, std::size_t size)
{
    std::size_t offset = 0U;
    while (offset < size) {
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const ssize_t sent = ::send(fd, data + offset, size - offset, flags);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{fd, POLLOUT, 0};
            if (::poll(&descriptor, 1U, 1000) > 0) {
                continue;
            }
        }
        return false;
    }
    return true;
}

class FrameCollector final : public FrameConsumer {
public:
    Result<void> on_frame(const FrameView& frame) noexcept override
    {
        if (count_ >= types_.size()) {
            return Result<void>::failure(Error::BUFFER_TOO_SMALL);
        }
        types_[count_++] = frame.header.type;
        return Result<void>::success();
    }

    std::size_t count() const noexcept { return count_; }
    MessageType type(std::size_t index) const noexcept { return types_[index]; }

private:
    std::array<MessageType, 8U> types_{};
    std::size_t count_ = 0U;
};

bool connect_pair(SocketListener& listener, const EndpointConfig& config,
                  SocketStream& client, SocketStream& server)
{
    auto connected = SocketStream::connect(config, Timeout{1000U});
    if (!connected) {
        return false;
    }
    auto accepted = listener.accept(Timeout{1000U});
    if (!accepted) {
        return false;
    }
    client = std::move(connected.value());
    server = std::move(accepted.value());
    return true;
}

bool test_private_endpoint_and_framing()
{
    TemporaryDirectory runtime(0700);
    CHECK(runtime.valid());
    const EndpointConfig config{runtime.path().c_str(), "private", kControlEndpointName,
                                EndpointAccess::private_user};
    const std::string product = runtime.path() + "/px4-userland";
    const std::string instance = product + "/private";
    const std::string endpoint = instance + "/control.sock";

    {
        auto listener_result = SocketListener::listen(config);
        CHECK(listener_result);
        SocketListener listener = std::move(listener_result.value());
        CHECK(std::strcmp(listener.endpoint_path(), endpoint.c_str()) == 0);
        CHECK(permissions(product) == 0700);
        CHECK(permissions(instance) == 0700);
        CHECK(permissions(endpoint) == 0600);
        CHECK(is_socket(endpoint));

        const auto empty_accept = listener.accept(Timeout{20U});
        CHECK(!empty_accept && empty_accept.error() == Error::TIMEOUT);

        SocketStream client;
        SocketStream server;
        CHECK(connect_pair(listener, config, client, server));
        CHECK((::fcntl(client.native_handle(), F_GETFD) & FD_CLOEXEC) != 0);
        CHECK((::fcntl(server.native_handle(), F_GETFD) & FD_CLOEXEC) != 0);

        const auto hello = make_hello_frame(1U);
        CHECK(!hello.empty());
        CHECK(raw_send_all(client.native_handle(), hello.data(), 3U));
        CHECK(raw_send_all(client.native_handle(), hello.data() + 3U, 9U));
        CHECK(raw_send_all(client.native_handle(), hello.data() + 12U,
                           hello.size() - 12U));

        std::array<std::uint8_t, 256U> framing_storage{};
        std::array<std::uint8_t, 7U> read_buffer{};
        StreamFramer framer(MutableByteView{framing_storage.data(), framing_storage.size()});
        FrameCollector collector;
        while (collector.count() == 0U) {
            const auto read = server.read_frames(
                MutableByteView{read_buffer.data(), read_buffer.size()}, framer, collector,
                Timeout{1000U});
            CHECK(read);
        }
        CHECK(collector.count() == 1U && collector.type(0U) == MessageType::HELLO);

        const auto list = make_empty_frame(MessageType::LIST, 2U);
        const auto status = make_empty_frame(MessageType::STATUS, 3U);
        std::vector<std::uint8_t> coalesced;
        coalesced.insert(coalesced.end(), list.begin(), list.end());
        coalesced.insert(coalesced.end(), status.begin(), status.end());
        CHECK(raw_send_all(client.native_handle(), coalesced.data(), coalesced.size()));
        std::array<std::uint8_t, 256U> large_read{};
        while (collector.count() < 3U) {
            const auto read = server.read_frames(
                MutableByteView{large_read.data(), large_read.size()}, framer, collector,
                Timeout{1000U});
            CHECK(read);
        }
        CHECK(collector.type(1U) == MessageType::LIST);
        CHECK(collector.type(2U) == MessageType::STATUS);

        CHECK(server.write_frame(ByteView{hello.data(), hello.size()}, Timeout{1000U}));
        std::vector<std::uint8_t> received(hello.size(), 0U);
        std::size_t received_size = 0U;
        while (received_size < received.size()) {
            const auto read = client.read_some(
                MutableByteView{received.data() + received_size,
                                received.size() - received_size},
                Timeout{1000U});
            CHECK(read);
            received_size += read.value();
        }
        CHECK(received == hello);

        const auto timed_out = client.read_some(
            MutableByteView{received.data(), received.size()}, Timeout{20U});
        CHECK(!timed_out && timed_out.error() == Error::TIMEOUT);

        server.close();
        const auto disconnected = client.read_some(
            MutableByteView{received.data(), received.size()}, Timeout{1000U});
        CHECK(!disconnected && disconnected.error() == Error::DISCONNECTED);
        const auto broken_write = client.write_frame(
            ByteView{hello.data(), hello.size()}, Timeout{1000U});
        CHECK(!broken_write && broken_write.error() == Error::DISCONNECTED);
    }
    CHECK(!std::filesystem::exists(endpoint));
    CHECK(!std::filesystem::exists(instance));
    CHECK(!std::filesystem::exists(product));
    return true;
}

bool test_xdg_runtime_default()
{
    TemporaryDirectory runtime(0700);
    CHECK(runtime.valid());
    EnvironmentRestore restore("XDG_RUNTIME_DIR");
    CHECK(::setenv("XDG_RUNTIME_DIR", runtime.path().c_str(), 1) == 0);
    const EndpointConfig config{nullptr, "xdg-default", kControlEndpointName,
                                EndpointAccess::private_user};
    auto listener = SocketListener::listen(config);
    CHECK(listener);
    const std::string expected =
        runtime.path() + "/px4-userland/xdg-default/control.sock";
    CHECK(std::strcmp(listener.value().endpoint_path(), expected.c_str()) == 0);
    return true;
}

bool test_rejects_overlong_unix_endpoint()
{
    std::string runtime(sizeof(sockaddr_un{}.sun_path), 'r');
    runtime[0] = '/';
    const EndpointConfig config{runtime.c_str(), "overlong", kControlEndpointName,
                                EndpointAccess::private_user};
    const auto listener = SocketListener::listen(config);
    CHECK(!listener && listener.error() == Error::INVALID_ARGUMENT);
    return true;
}

std::vector<std::uint8_t> make_large_ts_frame()
{
    constexpr std::size_t ts_size = 188U * 4000U;
    std::vector<std::uint8_t> ts(ts_size, 0x47U);
    std::vector<std::uint8_t> payload(20U + ts.size(), 0U);
    const auto payload_size = encode_payload(
        TsDataEventPayload{7U, 0U, ByteView{ts.data(), ts.size()}},
        MutableByteView{payload.data(), payload.size()});
    if (!payload_size) {
        return {};
    }
    std::vector<std::uint8_t> frame(kFrameHeaderSize + payload_size.value(), 0U);
    const FrameHeader header{kProtocolMajor, kProtocolMinor, MessageType::TS_DATA,
                             MessageKind::event, 0U,
                             static_cast<std::uint32_t>(payload_size.value())};
    const auto encoded = encode_frame(
        header, ByteView{payload.data(), payload_size.value()},
        MutableByteView{frame.data(), frame.size()});
    return encoded ? frame : std::vector<std::uint8_t>{};
}

bool test_partial_write_and_write_timeout()
{
    TemporaryDirectory runtime(0700);
    CHECK(runtime.valid());
    const EndpointConfig config{runtime.path().c_str(), "partial", kStreamEndpointName,
                                EndpointAccess::private_user};
    auto listener_result = SocketListener::listen(config);
    CHECK(listener_result);
    SocketListener listener = std::move(listener_result.value());
    SocketStream client;
    SocketStream server;
    CHECK(connect_pair(listener, config, client, server));

    int send_buffer = 1024;
    CHECK(::setsockopt(client.native_handle(), SOL_SOCKET, SO_SNDBUF, &send_buffer,
                       static_cast<socklen_t>(sizeof(send_buffer))) == 0);
    const auto frame = make_large_ts_frame();
    CHECK(!frame.empty());
    std::vector<std::uint8_t> received(frame.size(), 0U);
    std::atomic<bool> reader_ok{true};
    std::thread reader([&]() {
        std::size_t offset = 0U;
        while (offset < received.size()) {
            const auto read = server.read_some(
                MutableByteView{received.data() + offset, received.size() - offset},
                Timeout{5000U});
            if (!read) {
                reader_ok.store(false);
                return;
            }
            offset += read.value();
        }
    });
    const auto written = client.write_frame(ByteView{frame.data(), frame.size()},
                                            Timeout{5000U});
    reader.join();
    CHECK(written && reader_ok.load() && received == frame);

    client.close();
    server.close();
    CHECK(connect_pair(listener, config, client, server));
    CHECK(::setsockopt(client.native_handle(), SOL_SOCKET, SO_SNDBUF, &send_buffer,
                       static_cast<socklen_t>(sizeof(send_buffer))) == 0);
    std::array<std::uint8_t, 4096U> filler{};
    while (true) {
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const ssize_t sent = ::send(client.native_handle(), filler.data(), filler.size(), flags);
        if (sent > 0 || (sent < 0 && errno == EINTR)) {
            continue;
        }
        CHECK(sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        break;
    }
    const auto hello = make_hello_frame(9U);
    const auto timed_out = client.write_frame(ByteView{hello.data(), hello.size()},
                                              Timeout{20U});
    CHECK(!timed_out && timed_out.error() == Error::TIMEOUT);
    return true;
}

int create_bound_stale_socket(const std::string& path, mode_t mode)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        if (fd >= 0) {
            (void)::close(fd);
        }
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    const socklen_t length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1U);
#if defined(__APPLE__)
    address.sun_len = static_cast<decltype(address.sun_len)>(length);
#endif
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), length) != 0 ||
        ::chmod(path.c_str(), mode) != 0) {
        (void)::close(fd);
        return -1;
    }
    return fd;
}

bool make_endpoint_directories(const std::string& runtime, mode_t mode,
                               std::string& instance)
{
    const std::string product = runtime + "/px4-userland";
    instance = product + "/security";
    return ::mkdir(product.c_str(), mode) == 0 &&
           ::chmod(product.c_str(), mode) == 0 &&
           ::mkdir(instance.c_str(), mode) == 0 &&
           ::chmod(instance.c_str(), mode) == 0;
}

bool test_permissions_and_unsafe_paths()
{
    {
        TemporaryDirectory runtime(0750);
        CHECK(runtime.valid());
        const EndpointConfig config{runtime.path().c_str(), "group", kControlEndpointName,
                                    EndpointAccess::shared_group};
        auto listener = SocketListener::listen(config);
        CHECK(listener);
        const std::string product = runtime.path() + "/px4-userland";
        const std::string instance = product + "/group";
        CHECK(permissions(product) == 0750);
        CHECK(permissions(instance) == 0750);
        CHECK(permissions(listener.value().endpoint_path()) == 0660);
    }
    {
        TemporaryDirectory runtime(0755);
        CHECK(runtime.valid());
        const EndpointConfig config{runtime.path().c_str(), "bad", kControlEndpointName,
                                    EndpointAccess::private_user};
        const auto listener = SocketListener::listen(config);
        CHECK(!listener && listener.error() == Error::INVALID_ARGUMENT);
    }
    {
        TemporaryDirectory runtime(0700);
        TemporaryDirectory target(0700);
        CHECK(runtime.valid() && target.valid());
        const std::string product = runtime.path() + "/px4-userland";
        CHECK(::symlink(target.path().c_str(), product.c_str()) == 0);
        const EndpointConfig config{runtime.path().c_str(), "security",
                                    kControlEndpointName,
                                    EndpointAccess::private_user};
        const auto listener = SocketListener::listen(config);
        CHECK(!listener && listener.error() == Error::INVALID_ARGUMENT);
        struct stat status {};
        CHECK(::lstat(product.c_str(), &status) == 0 && S_ISLNK(status.st_mode));
    }
    {
        TemporaryDirectory runtime(0700);
        CHECK(runtime.valid());
        const std::string product = runtime.path() + "/px4-userland";
        const int file = ::open(product.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        CHECK(file >= 0);
        (void)::close(file);
        const EndpointConfig config{runtime.path().c_str(), "security",
                                    kControlEndpointName,
                                    EndpointAccess::private_user};
        const auto listener = SocketListener::listen(config);
        CHECK(!listener && listener.error() == Error::INVALID_ARGUMENT);
    }
    {
        TemporaryDirectory runtime(0700);
        TemporaryDirectory target(0700);
        CHECK(runtime.valid() && target.valid());
        std::string instance;
        CHECK(make_endpoint_directories(runtime.path(), 0700, instance));
        const std::string endpoint = instance + "/control.sock";
        CHECK(::symlink(target.path().c_str(), endpoint.c_str()) == 0);
        const EndpointConfig config{runtime.path().c_str(), "security",
                                    kControlEndpointName,
                                    EndpointAccess::private_user};
        const auto listener = SocketListener::listen(config);
        CHECK(!listener && listener.error() == Error::INVALID_ARGUMENT);
        struct stat status {};
        CHECK(::lstat(endpoint.c_str(), &status) == 0 && S_ISLNK(status.st_mode));
    }
    {
        TemporaryDirectory runtime(0700);
        CHECK(runtime.valid());
        std::string instance;
        CHECK(make_endpoint_directories(runtime.path(), 0700, instance));
        const std::string endpoint = instance + "/control.sock";
        const int stale = create_bound_stale_socket(endpoint, 0666);
        CHECK(stale >= 0);
        (void)::close(stale);
        const EndpointConfig config{runtime.path().c_str(), "security",
                                    kControlEndpointName,
                                    EndpointAccess::private_user};
        const auto listener = SocketListener::listen(config);
        CHECK(!listener && listener.error() == Error::INVALID_ARGUMENT);
        CHECK(is_socket(endpoint) && permissions(endpoint) == 0666);
    }
    if (::geteuid() == 0) {
        TemporaryDirectory runtime(0700);
        CHECK(runtime.valid());
        const std::string product = runtime.path() + "/px4-userland";
        CHECK(::mkdir(product.c_str(), 0700) == 0);
        CHECK(::chown(product.c_str(), 65534U, 65534U) == 0);
        const EndpointConfig config{runtime.path().c_str(), "security",
                                    kControlEndpointName,
                                    EndpointAccess::private_user};
        const auto listener = SocketListener::listen(config);
        CHECK(!listener && listener.error() == Error::INVALID_ARGUMENT);
        struct stat status {};
        CHECK(::lstat(product.c_str(), &status) == 0 && status.st_uid != ::geteuid());
    }

    std::string long_runtime = "/tmp/";
    long_runtime.append(500U, 'x');
    const EndpointConfig long_config{long_runtime.c_str(), "instance",
                                     kControlEndpointName,
                                     EndpointAccess::private_user};
    const auto long_listener = SocketListener::listen(long_config);
    CHECK(!long_listener && long_listener.error() == Error::INVALID_ARGUMENT);

    TemporaryDirectory runtime(0700);
    CHECK(runtime.valid());
    const EndpointConfig traversal{runtime.path().c_str(), "../bad",
                                   kControlEndpointName,
                                   EndpointAccess::private_user};
    CHECK(!SocketListener::listen(traversal));
    const std::string too_long_instance(81U, 'x');
    const EndpointConfig oversized{runtime.path().c_str(), too_long_instance.c_str(),
                                    kControlEndpointName, EndpointAccess::private_user};
    const auto oversized_listener = SocketListener::listen(oversized);
    CHECK(!oversized_listener && oversized_listener.error() == Error::INVALID_ARGUMENT);
    return true;
}

bool test_stale_active_and_cleanup_identity()
{
    {
        TemporaryDirectory runtime(0700);
        CHECK(runtime.valid());
        std::string instance;
        CHECK(make_endpoint_directories(runtime.path(), 0700, instance));
        const EndpointConfig config{runtime.path().c_str(), "security",
                                    kControlEndpointName,
                                    EndpointAccess::private_user};
        auto listener_result = SocketListener::listen(config);
        CHECK(listener_result);
        SocketListener listener = std::move(listener_result.value());
        const std::string product = runtime.path() + "/px4-userland";
        const std::string expected_instance = instance;
        listener.close();
        CHECK(!std::filesystem::exists(expected_instance + "/control.sock"));
        CHECK(std::filesystem::exists(product));
        CHECK(std::filesystem::exists(expected_instance));
    }
    {
        TemporaryDirectory runtime(0700);
        CHECK(runtime.valid());
        std::string instance;
        CHECK(make_endpoint_directories(runtime.path(), 0700, instance));
        const std::string endpoint = instance + "/control.sock";
        const int stale = create_bound_stale_socket(endpoint, 0600);
        CHECK(stale >= 0);
        (void)::close(stale);

        const EndpointConfig config{runtime.path().c_str(), "security",
                                    kControlEndpointName,
                                    EndpointAccess::private_user};
        auto listener = SocketListener::listen(config);
        CHECK(listener);
        SocketStream client;
        SocketStream server;
        CHECK(connect_pair(listener.value(), config, client, server));
        const auto second = SocketListener::listen(config);
        CHECK(!second && second.error() == Error::BUSY);
        CHECK(is_socket(endpoint));
    }
    {
        TemporaryDirectory runtime(0700);
        CHECK(runtime.valid());
        const EndpointConfig config{runtime.path().c_str(), "replace",
                                    kControlEndpointName,
                                    EndpointAccess::private_user};
        auto listener_result = SocketListener::listen(config);
        CHECK(listener_result);
        SocketListener listener = std::move(listener_result.value());
        const std::string endpoint = listener.endpoint_path();
        const std::uint64_t owned_inode = inode_of(endpoint);
        CHECK(::unlink(endpoint.c_str()) == 0);
        const int replacement = create_bound_stale_socket(endpoint, 0600);
        CHECK(replacement >= 0 && inode_of(endpoint) != owned_inode);
        listener.close();
        CHECK(is_socket(endpoint));
        (void)::close(replacement);
        CHECK(::unlink(endpoint.c_str()) == 0);
    }
    {
        TemporaryDirectory runtime(0700);
        CHECK(runtime.valid());
        const EndpointConfig config{runtime.path().c_str(), "contents",
                                    kControlEndpointName,
                                    EndpointAccess::private_user};
        auto listener_result = SocketListener::listen(config);
        CHECK(listener_result);
        SocketListener listener = std::move(listener_result.value());
        const std::string instance = runtime.path() + "/px4-userland/contents";
        const std::string unrelated = instance + "/keep";
        const int file = ::open(unrelated.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        CHECK(file >= 0);
        (void)::close(file);
        listener.close();
        CHECK(std::filesystem::exists(unrelated));
        CHECK(std::filesystem::exists(instance));
    }
    return true;
}

bool test_argument_and_peer_failures()
{
    SocketStream empty;
    std::array<std::uint8_t, 32U> buffer{};
    CHECK(!empty.read_some(MutableByteView{buffer.data(), buffer.size()}, Timeout{1U}));
    CHECK(!empty.write_frame(ByteView{buffer.data(), buffer.size()}, Timeout{1U}));

    TemporaryDirectory runtime(0700);
    CHECK(runtime.valid());
    const EndpointConfig config{runtime.path().c_str(), "arguments",
                                kControlEndpointName,
                                EndpointAccess::private_user};
    auto listener_result = SocketListener::listen(config);
    CHECK(listener_result);
    SocketListener listener = std::move(listener_result.value());
    SocketStream client;
    SocketStream server;
    CHECK(connect_pair(listener, config, client, server));

    CHECK(!client.read_some(MutableByteView{nullptr, 0U}, Timeout{1U}));
    const auto hello = make_hello_frame(1U);
    CHECK(!client.write_frame(ByteView{hello.data(), hello.size() - 1U}, Timeout{10U}));
    const auto no_data = server.read_some(
        MutableByteView{buffer.data(), buffer.size()}, Timeout{20U});
    CHECK(!no_data && no_data.error() == Error::TIMEOUT);

    client.close();
    const auto peer_closed = server.read_some(
        MutableByteView{buffer.data(), buffer.size()}, Timeout{1000U});
    CHECK(!peer_closed && peer_closed.error() == Error::DISCONNECTED);
    return true;
}

bool test_serial_endpoint_interlock_and_cleanup()
{
    TemporaryDirectory runtime(0700);
    CHECK(runtime.valid());
    constexpr const char* serial = "000000000000001";
    const EndpointConfig legacy{runtime.path().c_str(), serial,
                                kControlEndpointName, EndpointAccess::private_user};
    const EndpointConfig custom_a{runtime.path().c_str(), "m1-at-port2",
                                  kControlEndpointName, EndpointAccess::private_user};
    const EndpointConfig custom_b{runtime.path().c_str(), "s1-at-port4",
                                  kControlEndpointName, EndpointAccess::private_user};
    const std::string lock_path = runtime.path() + "/.px4-userland-" + serial + ".lock";

    auto serial_lease = SerialEndpointLease::acquire(legacy, serial);
    CHECK(serial_lease && serial_lease.value().valid());
    CHECK(permissions(lock_path) == 0600);
    CHECK(SerialEndpointLease::acquire(custom_a, serial).error() == Error::BUSY);
    auto legacy_listener = SocketListener::listen(legacy);
    CHECK(legacy_listener);
    legacy_listener.value().close();
    serial_lease.value().close();
    CHECK(!std::filesystem::exists(lock_path));

    auto first = SerialEndpointLease::acquire(custom_a, serial);
    auto second = SerialEndpointLease::acquire(custom_b, serial);
    CHECK(first && second);
    CHECK(SerialEndpointLease::acquire(legacy, serial).error() == Error::BUSY);
    first.value().close();
    CHECK(std::filesystem::exists(lock_path));
    CHECK(SerialEndpointLease::acquire(legacy, serial).error() == Error::BUSY);
    second.value().close();
    CHECK(!std::filesystem::exists(lock_path));
    CHECK(std::filesystem::is_empty(runtime.path()));
    CHECK(::rmdir(runtime.path().c_str()) == 0);

    TemporaryDirectory group_runtime(0750);
    CHECK(group_runtime.valid());
    const EndpointConfig group_endpoint{group_runtime.path().c_str(), "q3-port1",
                                        kControlEndpointName, EndpointAccess::shared_group};
    auto group_lease = SerialEndpointLease::acquire(group_endpoint, serial);
    CHECK(group_lease && group_lease.value().valid());
    group_lease.value().close();
    CHECK(std::filesystem::is_empty(group_runtime.path()));

    TemporaryDirectory racing_runtime(0700);
    CHECK(racing_runtime.valid());
    const EndpointConfig racing_legacy{racing_runtime.path().c_str(), serial,
                                       kControlEndpointName, EndpointAccess::private_user};
    const EndpointConfig racing_custom{racing_runtime.path().c_str(), "port-4",
                                       kControlEndpointName, EndpointAccess::private_user};
    std::atomic<int> started{0};
    std::atomic<int> legacy_result{-1};
    std::atomic<int> custom_result{-1};
    std::atomic<bool> release{false};
    const auto race = [&](const EndpointConfig& config, std::atomic<int>& outcome) {
        started.fetch_add(1);
        while (started.load() != 2) std::this_thread::yield();
        auto lease = SerialEndpointLease::acquire(config, serial);
        outcome.store(lease ? 1 : lease.error() == Error::BUSY ? 0 : -2);
        while (!release.load()) std::this_thread::yield();
    };
    std::thread legacy_thread(race, std::cref(racing_legacy), std::ref(legacy_result));
    std::thread custom_thread(race, std::cref(racing_custom), std::ref(custom_result));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((legacy_result.load() == -1 || custom_result.load() == -1) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool one_winner =
        (legacy_result.load() == 1 && custom_result.load() == 0) ||
        (legacy_result.load() == 0 && custom_result.load() == 1);
    release.store(true);
    legacy_thread.join();
    custom_thread.join();
    CHECK(one_winner);
    CHECK(std::filesystem::is_empty(racing_runtime.path()));
    return true;
}

}  // namespace

bool run_posix_ipc_tests()
{
    return test_ownership_group_authorization() &&
           test_private_endpoint_and_framing() &&
           test_xdg_runtime_default() &&
           test_rejects_overlong_unix_endpoint() &&
           test_partial_write_and_write_timeout() &&
           test_permissions_and_unsafe_paths() &&
           test_stale_active_and_cleanup_identity() &&
           test_argument_and_peer_failures() &&
           test_serial_endpoint_interlock_and_cleanup();
}
