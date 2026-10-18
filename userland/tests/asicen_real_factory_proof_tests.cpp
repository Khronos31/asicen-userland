// SPDX-License-Identifier: GPL-2.0-or-later
// Ordinary live-factory proof with only the bottom USB API and transfers shimmed.
#define main libusb_acquisition_fixture_main
#include "libusb_acquisition_tests.cpp"
#undef main
#undef CHECK

#define main libusb_capture_fixture_main
#include "libusb_capture_lifecycle_tests.cpp"
#undef main
#undef CHECK

#include "asicen/card_only_service.h"
#include "asicen/libusb_hardware_backend.h"
#include "transport_transform_reference.h"
#include "dtv/userland/control_connection.h"
#include "dtv/userland/records.h"
#include "dtv/userland/wire.h"
#include "px4/control_server.h"
#include "px4_ts_core.h"
#include "px4_ts_posix.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

namespace wire = dtv::userland::wire;
using namespace px4::userland::ipc::posix;
using namespace px4::userland;
using namespace dtv::userland;
using asicen::LibusbW3u3Hardware;
using Hooks = LibusbW3u3HardwareTestPeer::Hooks;

#define PROOF_CHECK(condition)                                                       \
    do {                                                                             \
        if (!(condition)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                            \
        }                                                                            \
    } while (false)

std::atomic<bool> factory_stop{false};
bool stop_factory() noexcept { return factory_stop.load(std::memory_order_acquire); }

template <typename Predicate>
bool wait_for(std::chrono::milliseconds bound, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

class CallerPipe final {
public:
    CallerPipe()
    {
        int descriptors[2]{};
        if (::pipe(descriptors) == 0) {
            read_ = descriptors[0];
            write_ = descriptors[1];
        }
    }
    ~CallerPipe()
    {
        if (read_ >= 0) ::close(read_);
        if (write_ >= 0) ::close(write_);
    }
    int read_fd() const noexcept { return read_; }
    int write_fd() const noexcept { return write_; }
private:
    int read_ = -1;
    int write_ = -1;
};

class RuntimeDirectory final {
public:
    explicit RuntimeDirectory(std::filesystem::path path) : path_(std::move(path)) {}
    ~RuntimeDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
private:
    std::filesystem::path path_;
};

class FactoryRunner final {
public:
    FactoryRunner(LibusbW3u3Hardware& hardware, const char* directory, const char* instance)
        : thread_([this, &hardware, directory, instance] {
              result_.store(asicen::run_live_card_stream_server(
                                hardware, directory, instance, false, nullptr, stop_factory),
                            std::memory_order_release);
              done_.store(true, std::memory_order_release);
          })
    {
    }
    ~FactoryRunner() { stop_and_join(); }
    bool done() const noexcept { return done_.load(std::memory_order_acquire); }
    int result() const noexcept { return result_.load(std::memory_order_acquire); }
    bool stop_and_join()
    {
        factory_stop.store(true, std::memory_order_release);
        if (!thread_.joinable()) return true;
        if (!wait_for(std::chrono::seconds(5), [this] { return done(); })) {
            std::fprintf(stderr, "live factory did not stop within bound\n");
            std::_Exit(124);
        }
        thread_.join();
        return true;
    }
private:
    std::atomic<bool> done_{false};
    std::atomic<int> result_{-1};
    std::thread thread_;
};

class OwnedFactoryRunner final {
public:
    explicit OwnedFactoryRunner(EnclosureContext& context)
        : thread_([this, &context] {
              const auto served = asicen::run_live_card_stream_server(
                  context, nullptr, stop_factory);
              result_.store(served.exit_status, std::memory_order_release);
              deadline_expired_.store(served.close_deadline_expired,
                                       std::memory_order_release);
              done_.store(true, std::memory_order_release);
          })
    {
    }
    ~OwnedFactoryRunner() { stop_and_join(); }
    bool done() const noexcept { return done_.load(std::memory_order_acquire); }
    int result() const noexcept { return result_.load(std::memory_order_acquire); }
    bool deadline_expired() const noexcept
    {
        return deadline_expired_.load(std::memory_order_acquire);
    }
    bool stop_and_join()
    {
        factory_stop.store(true, std::memory_order_release);
        if (!thread_.joinable()) return true;
        if (!wait_for(std::chrono::seconds(5), [this] { return done(); })) {
            std::fprintf(stderr, "owned live factory did not stop within bound\n");
            std::_Exit(124);
        }
        thread_.join();
        return true;
    }
private:
    std::atomic<bool> done_{false};
    std::atomic<int> result_{-1};
    std::atomic<bool> deadline_expired_{false};
    std::thread thread_;
};

struct GatedFakeUsb final {
    static std::atomic<GatedFakeUsb*> active;
    FakeUsb usb;
    std::atomic<bool> block_next_control{false};
    std::atomic<bool> control_entered{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;

    static int control_function_hook(void* context, std::uint8_t function,
                                     const ControlTransfer& transfer, unsigned char* response)
    {
        auto* gate = active.load(std::memory_order_acquire);
        if (gate != nullptr && context == &gate->usb &&
            gate->block_next_control.exchange(false, std::memory_order_acq_rel)) {
            std::unique_lock<std::mutex> lock(gate->mutex);
            gate->control_entered.store(true, std::memory_order_release);
            gate->changed.notify_all();
            gate->changed.wait(lock, [&] { return gate->released; });
        }
        return FakeUsb::control_function_hook(context, function, transfer, response);
    }

    Hooks hooks()
    {
        Hooks value = usb.hooks();
        active.store(this, std::memory_order_release);
        value.control_function = &GatedFakeUsb::control_function_hook;
        return value;
    }

    void release() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
};

std::atomic<GatedFakeUsb*> GatedFakeUsb::active{nullptr};

class GateThreadJoin final {
public:
    GateThreadJoin(GatedFakeUsb& gate, std::thread& thread) noexcept
        : gate_(gate), thread_(thread) {}
    ~GateThreadJoin()
    {
        gate_.release();
        if (thread_.joinable()) thread_.join();
    }
    void join()
    {
        if (thread_.joinable()) thread_.join();
    }
private:
    GatedFakeUsb& gate_;
    std::thread& thread_;
};

class RecordingClock final : public px4::userland::cli::Px4TsClock {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
};

class RecordingSignal final : public px4::userland::cli::Px4TsSignal {
public:
    bool stop_requested() const noexcept override { return stopped.load(); }
    std::atomic<bool> stopped{false};
};

class ProofClient final {
public:
    struct Reply {
        wire::Header header{};
        std::vector<std::uint8_t> payload;
        ByteView view() const noexcept { return {payload.data(), payload.size()}; }
    };
    explicit ProofClient(SocketStream stream)
        : stream_(std::move(stream)), storage_(new std::array<std::uint8_t, wire::kMaxControlPayload>()),
          receiver_(MutableByteView{storage_->data(), storage_->size()})
    {
    }
    static std::unique_ptr<ProofClient> connect(const EndpointConfig& endpoint)
    {
        auto stream = SocketStream::connect(endpoint, Timeout{250U});
        if (!stream) return nullptr;
        return std::unique_ptr<ProofClient>(new ProofClient(std::move(stream.value())));
    }
    template <typename Record>
    static std::vector<std::uint8_t> encode(
        const Record& value,
        Result<std::size_t> (*encoder)(const Record&, MutableByteView) noexcept)
    {
        std::vector<std::uint8_t> bytes(wire::kMaxControlPayload);
        const auto size = encoder(value, MutableByteView{bytes.data(), bytes.size()});
        bytes.resize(size ? size.value() : 0U);
        return bytes;
    }
    bool request(std::uint16_t opcode, const std::vector<std::uint8_t>& payload, Reply& out)
    {
        std::vector<std::uint8_t> bytes(wire::kHeaderSize + payload.size());
        wire::Header header;
        header.kind = wire::Kind::request;
        header.opcode = opcode;
        header.request_id = ++request_id_;
        header.payload_length = static_cast<std::uint32_t>(payload.size());
        const auto encoded = wire::encode_frame(
            header, ByteView{payload.data(), payload.size()}, MutableByteView{bytes.data(), bytes.size()});
        if (!encoded || !send_all(bytes.data(), bytes.size()) || !read_frame(out)) return false;
        return out.header.opcode == opcode && out.header.request_id == request_id_;
    }
    bool hello(wire::HelloResponse& out)
    {
        wire::HelloRequest value;
        value.min = wire::Version{1U, 0U};
        value.max = wire::Version{1U, 0U};
        Reply reply;
        if (!request(wire::kOpcodeHello, encode(value, wire::encode_hello_request), reply) ||
            reply.header.kind != wire::Kind::response) return false;
        auto decoded = wire::decode_hello_response(reply.view());
        if (!decoded) return false;
        out = decoded.value();
        epoch_ = out.epoch;
        return true;
    }
    bool list(wire::ListResponse& out)
    {
        wire::ListRequest value;
        value.epoch = epoch_;
        Reply reply;
        if (!request(wire::kOpcodeList, encode(value, wire::encode_list_request), reply) ||
            reply.header.kind != wire::Kind::response) return false;
        auto decoded = wire::decode_list_response(reply.view());
        if (!decoded) return false;
        out = decoded.value();
        return true;
    }
    bool status(Reply& reply, wire::StatusResponse* out = nullptr)
    {
        wire::StatusRequest value;
        value.epoch = epoch_;
        value.scope.kind = wire::ScopeKind::enclosure;
        if (!request(wire::kOpcodeStatus, encode(value, wire::encode_status_request), reply) ||
            reply.header.kind != wire::Kind::response) return false;
        if (out != nullptr) {
            auto decoded = wire::decode_status_response(reply.view());
            if (!decoded) return false;
            *out = decoded.value();
        }
        return true;
    }
    bool acquire(std::uint16_t receiver, Reply& reply)
    {
        wire::AcquireRequest value;
        value.epoch = epoch_;
        value.receiver_id = receiver;
        return request(wire::kOpcodeAcquire, encode(value, wire::encode_acquire_request), reply);
    }
    bool tune(std::uint64_t lease, std::uint64_t hz, wire::System system, Reply& reply,
              wire::Selection selection = wire::Selection::automatic,
              std::uint16_t selection_value = 0U, std::uint32_t timeout_ms = 3000U)
    {
        wire::TuneRequest value;
        value.epoch = epoch_;
        value.lease_id = lease;
        value.system = system;
        value.frequency_hz = hz;
        value.selection = selection;
        value.selection_value = selection_value;
        value.timeout_ms = timeout_ms;
        return request(wire::kOpcodeTune, encode(value, wire::encode_tune_request), reply);
    }
    bool release(std::uint64_t lease, Reply& reply)
    {
        wire::ReleaseRequest value;
        value.epoch = epoch_;
        value.lease_id = lease;
        return request(wire::kOpcodeRelease, encode(value, wire::encode_release_request), reply);
    }
    static std::uint32_t error_code(const Reply& reply)
    {
        auto decoded = wire::decode_error_response(reply.view());
        return decoded ? decoded.value().error.code : 0U;
    }
private:
    bool send_all(const std::uint8_t* data, std::size_t size)
    {
        std::size_t sent = 0U;
        while (sent < size) {
            const auto count = ::send(stream_.native_handle(), data + sent, size - sent, MSG_NOSIGNAL);
            if (count <= 0) return false;
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }
    bool read_frame(Reply& out)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
        while (collector_.frames.empty() && std::chrono::steady_clock::now() < deadline) {
            std::array<std::uint8_t, 4096U> buffer{};
            auto read = stream_.read_some(MutableByteView{buffer.data(), buffer.size()}, Timeout{20U});
            if (!read) {
                if (read.error() == Error::DISCONNECTED) return false;
                continue;
            }
            if (!receiver_.feed(ByteView{buffer.data(), read.value()}, collector_)) return false;
        }
        if (collector_.frames.empty()) return false;
        out = std::move(collector_.frames.front());
        collector_.frames.erase(collector_.frames.begin());
        return true;
    }
    struct Collector final : wire::FrameHandler {
        Result<void> on_frame(const wire::Frame& frame) noexcept override
        {
            Reply reply;
            reply.header = frame.header;
            reply.payload.assign(frame.payload.data, frame.payload.data + frame.payload.size);
            frames.push_back(std::move(reply));
            return Result<void>::success();
        }
        std::vector<Reply> frames;
    } collector_;
    SocketStream stream_;
    std::unique_ptr<std::array<std::uint8_t, wire::kMaxControlPayload>> storage_;
    wire::FrameReceiver receiver_;
    wire::Uuid epoch_{};
    std::uint32_t request_id_ = 0U;
};

std::string producer_version(const wire::Producer& producer)
{
    return std::string(producer.version.bytes.data(), producer.version.length);
}

bool run_model(std::uint16_t product, std::uint8_t expected_functions,
               std::uint8_t expected_receivers, wire::Uuid* epoch_out)
{
    FakeApi api;
    api.products_[0] = product;
    api.products_[1] = product;
    api.verify_wrapped_fds_closed = true;
    if (product == 0x0001U) api.addresses_ = {{2U, 2U}};
    asicen::LibusbContext context(api);
    PROOF_CHECK(context.initialize(true) == 0 && api.no_discovery);
    CallerPipe primary_fd;
    CallerPipe sibling_fd;
    PROOF_CHECK(primary_fd.read_fd() >= 0 && sibling_fd.read_fd() >= 0);
    FakeUsb usb;
    usb.terrestrial_b0 = 0xa9U;
    usb.satellite_tsid_ready = product == 0x0001U;
    Hooks hooks = usb.hooks();
    hooks.control_function = FakeUsb::control_function_hook;
    LibusbW3u3Hardware hardware(context.get(), primary_fd.read_fd(),
                                expected_functions == 2U ? sibling_fd.read_fd() : -1,
                                nullptr, api);
    asicen::LibusbW3u3HardwareTestPeer::hooks_only(hardware, &hooks);
    PROOF_CHECK(hardware.device_profile() != nullptr && hardware.claim());
    std::array<asicen::ClaimedUsbFunctionObservation, 2U> observations{};
    std::uint8_t observation_count = 0U;
    PROOF_CHECK(hardware.claimed_function_observations(observations, observation_count));
    PROOF_CHECK(observation_count == expected_functions);
    PROOF_CHECK(observations[0].vendor_id == 0x0b06U &&
                observations[0].product_id == product && observations[0].has_bus &&
                observations[0].bus == 1U && observations[0].has_address &&
                observations[0].address == 2U &&
                observations[0].port_path == std::vector<std::uint8_t>({2U, 1U}));
    if (expected_functions == 2U) {
        PROOF_CHECK(observations[1].vendor_id == 0x0b06U &&
                    observations[1].product_id == product && observations[1].has_bus &&
                    observations[1].bus == 1U && observations[1].has_address &&
                    observations[1].address == 3U &&
                    observations[1].port_path == std::vector<std::uint8_t>({2U, 2U}));
    }
    PROOF_CHECK(api.wrap_calls == expected_functions && api.snapshot_api_calls() > 0);
    PROOF_CHECK(::fcntl(primary_fd.read_fd(), F_GETFD) >= 0);
    PROOF_CHECK(expected_functions != 2U || ::fcntl(sibling_fd.read_fd(), F_GETFD) >= 0);

    const auto nonce = std::to_string(static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto directory = std::filesystem::temp_directory_path() / ("asicen-proof-" + nonce);
    PROOF_CHECK(std::filesystem::create_directory(directory));
    RuntimeDirectory cleanup_directory(directory);
    PROOF_CHECK(::chmod(directory.c_str(), 0700) == 0);
    factory_stop.store(false, std::memory_order_release);
    const std::string instance = "proof-" + nonce.substr(nonce.size() > 8U ? nonce.size() - 8U : 0U);
    FactoryRunner runner(hardware, directory.c_str(), instance.c_str());
    const EndpointConfig endpoint{directory.c_str(), instance.c_str(),
                                  kControlEndpointName, EndpointAccess::private_user};
    std::unique_ptr<ProofClient> client;
    PROOF_CHECK(wait_for(std::chrono::seconds(8), [&] {
        if (runner.done()) return true;
        client = ProofClient::connect(endpoint);
        return client != nullptr;
    }));
    PROOF_CHECK(client != nullptr && !runner.done());

    wire::HelloResponse hello;
    wire::ListResponse list;
    PROOF_CHECK(client->hello(hello));
    PROOF_CHECK(client->list(list));
    PROOF_CHECK(hello.epoch == list.epoch);
    PROOF_CHECK(std::string(hello.producer.product.bytes.data(), hello.producer.product.length) ==
                "asicen-userland");
    PROOF_CHECK(producer_version(hello.producer) == ASICEN_PRODUCT_VERSION);
    PROOF_CHECK(producer_version(list.producer) == ASICEN_PRODUCT_VERSION);
    PROOF_CHECK(list.functions.count == expected_functions);
    PROOF_CHECK(list.topology.expected_function_count.present &&
                list.topology.expected_function_count.value == expected_functions);
    PROOF_CHECK(list.receivers.present && list.receivers.value.count == expected_receivers);
    PROOF_CHECK(list.readers.present && list.readers.value.count == 1U &&
                list.readers.value.items[0].id == 0U &&
                list.readers.value.items[0].binding.present &&
                list.readers.value.items[0].binding.value.function_id == 0U &&
                list.readers.value.items[0].binding.value.local_id == 0U);
    PROOF_CHECK(list.enclosure.present &&
                list.enclosure.value.identity.serial_state == wire::IdentitySerialState::absent);
    PROOF_CHECK(list.functions.items[0].vid == 0x0b06U && list.functions.items[0].pid == product);
    PROOF_CHECK(list.functions.items[0].location.bus.present &&
                list.functions.items[0].location.bus.value == 1U);
    PROOF_CHECK(list.functions.items[0].location.address.present &&
                list.functions.items[0].location.address.value == 2U);
    PROOF_CHECK(list.functions.items[0].location.usb_path.present &&
                std::string(list.functions.items[0].location.usb_path.value.bytes.data(),
                            list.functions.items[0].location.usb_path.value.length) == "1-2.1");
    PROOF_CHECK(list.functions.items[0].serial_state == wire::SerialState::absent);
    if (expected_functions == 2U) {
        PROOF_CHECK(list.functions.items[1].location.address.present &&
                    list.functions.items[1].location.address.value == 3U);
        PROOF_CHECK(list.functions.items[1].serial_state == wire::SerialState::absent);
        PROOF_CHECK(list.functions.items[1].location.usb_path.present &&
                    std::string(list.functions.items[1].location.usb_path.value.bytes.data(),
                                list.functions.items[1].location.usb_path.value.length) == "1-2.2");
        PROOF_CHECK(list.receivers.value.items[0].binding.present &&
                    list.receivers.value.items[0].binding.value.function_id == 0U);
        PROOF_CHECK(list.receivers.value.items[1].binding.present &&
                    list.receivers.value.items[1].binding.value.function_id == 0U);
    } else {
        PROOF_CHECK(list.receivers.value.items[0].systems.mask == 3U);
    }

    const int usb_controls_before_status = static_cast<int>(usb.controls.size());
    const int acquisition_calls_before_status = api.snapshot_api_calls();
    wire::StatusResponse status;
    ProofClient::Reply status_reply;
    PROOF_CHECK(client->status(status_reply, &status));
    PROOF_CHECK(status.producer.product.length == hello.producer.product.length);
    PROOF_CHECK(producer_version(status.producer) == ASICEN_PRODUCT_VERSION);
    PROOF_CHECK(status.context.epoch == hello.epoch);
    PROOF_CHECK(status.readers.present && status.readers.value.count == 1U);
    PROOF_CHECK(status.readers.value.items[0].card.state == wire::CardState::unknown);
    PROOF_CHECK(status.readers.value.items[0].card.reason ==
                static_cast<std::uint8_t>(wire::UnknownReason::not_sampled));
    PROOF_CHECK(client->status(status_reply));
    PROOF_CHECK(static_cast<int>(usb.controls.size()) == usb_controls_before_status);
    PROOF_CHECK(api.snapshot_api_calls() == acquisition_calls_before_status);

    if (expected_functions == 2U) {
        auto second_client = ProofClient::connect(endpoint);
        PROOF_CHECK(second_client != nullptr);
        wire::HelloResponse second_hello;
        PROOF_CHECK(second_client->hello(second_hello));
        PROOF_CHECK(second_hello.epoch == hello.epoch);
        ProofClient::Reply first_acquire;
        ProofClient::Reply contended_acquire;
        PROOF_CHECK(client->acquire(0U, first_acquire));
        PROOF_CHECK(first_acquire.header.kind == wire::Kind::response);
        auto first_lease = wire::decode_acquire_response(first_acquire.view());
        PROOF_CHECK(first_lease.has_value());
        PROOF_CHECK(second_client->acquire(1U, contended_acquire));
        PROOF_CHECK(contended_acquire.header.kind == wire::Kind::error_response);
        PROOF_CHECK(ProofClient::error_code(contended_acquire) ==
                    static_cast<std::uint32_t>(Error::BUSY));
        ProofClient::Reply release_reply;
        PROOF_CHECK(client->release(first_lease.value().lease_id, release_reply));
        PROOF_CHECK(release_reply.header.kind == wire::Kind::response);
    }

    const std::uint16_t tune_receiver = expected_functions == 2U ? 1U : 0U;
    ProofClient::Reply acquire_reply;
    PROOF_CHECK(client->acquire(tune_receiver, acquire_reply));
    PROOF_CHECK(acquire_reply.header.kind == wire::Kind::response);
    auto acquired = wire::decode_acquire_response(acquire_reply.view());
    PROOF_CHECK(acquired.has_value() && acquired.value().epoch == hello.epoch);
    const auto lease = acquired.value().lease_id;
    ProofClient::Reply tune_reply;
    PROOF_CHECK(client->tune(lease, 473142857ULL, wire::System::isdb_t, tune_reply));
    if (tune_reply.header.kind != wire::Kind::response) {
        std::fprintf(stderr, "strict TUNE error code=%u\n", ProofClient::error_code(tune_reply));
    }
    PROOF_CHECK(tune_reply.header.kind == wire::Kind::response);
    auto tuned = wire::decode_tune_result(tune_reply.view());
    PROOF_CHECK(tuned.has_value() && tuned.value().requested_hz == 473142857ULL);
    ProofClient::Reply release_reply;
    PROOF_CHECK(client->release(lease, release_reply));
    PROOF_CHECK(release_reply.header.kind == wire::Kind::response);

    if (product == 0x0001U) {
        ProofClient::Reply satellite_acquire_reply;
        PROOF_CHECK(client->acquire(tune_receiver, satellite_acquire_reply));
        PROOF_CHECK(satellite_acquire_reply.header.kind == wire::Kind::response);
        auto satellite_acquired = wire::decode_acquire_response(satellite_acquire_reply.view());
        PROOF_CHECK(satellite_acquired.has_value());
        const auto satellite_lease = satellite_acquired.value().lease_id;
        ProofClient::Reply satellite_tune_reply;
        PROOF_CHECK(client->tune(satellite_lease, 1049480000ULL, wire::System::isdb_s,
                                 satellite_tune_reply, wire::Selection::slot, 0U, 30000U));
        if (satellite_tune_reply.header.kind != wire::Kind::response)
            std::fprintf(stderr, "strict satellite TUNE error code=%u\n",
                         ProofClient::error_code(satellite_tune_reply));
        PROOF_CHECK(satellite_tune_reply.header.kind == wire::Kind::response);
        auto satellite_tuned = wire::decode_tune_result(satellite_tune_reply.view());
        PROOF_CHECK(satellite_tuned.has_value() &&
                    satellite_tuned.value().requested_hz == 1049480000ULL);
        ProofClient::Reply satellite_release_reply;
        PROOF_CHECK(client->release(satellite_lease, satellite_release_reply));
        PROOF_CHECK(satellite_release_reply.header.kind == wire::Kind::response);
    }

    if (epoch_out != nullptr) *epoch_out = hello.epoch;
    client.reset();
    runner.stop_and_join();
    PROOF_CHECK(runner.result() == 0);
    PROOF_CHECK(hardware.shutdown());
    PROOF_CHECK(hardware.release());
    PROOF_CHECK(api.wrapped_fds.size() == expected_functions);
    for (const int fd : api.wrapped_fds) {
        PROOF_CHECK(fd >= 0 && fd != primary_fd.read_fd() && fd != sibling_fd.read_fd());
        PROOF_CHECK(::fcntl(fd, F_GETFD) == -1);
    }
    PROOF_CHECK(::fcntl(primary_fd.read_fd(), F_GETFD) >= 0);
    PROOF_CHECK(expected_functions != 2U || ::fcntl(sibling_fd.read_fd(), F_GETFD) >= 0);
    context.close();
    PROOF_CHECK(api.valid);
    if (expected_functions == 1U)
        PROOF_CHECK(cleanup_order(api, {"release1", "close1", "exit"}));
    else
        PROOF_CHECK(cleanup_order(api, {"release2", "release1", "close1", "close2", "exit"}));
    return true;
}

bool test_actual_s3u_and_w3u3_live_factories()
{
    wire::Uuid first_epoch{};
    wire::Uuid second_epoch{};
    PROOF_CHECK(run_model(0x0001U, 1U, 1U, &first_epoch));
    PROOF_CHECK(run_model(0x0005U, 2U, 2U, &second_epoch));
    PROOF_CHECK(first_epoch != second_epoch);
    return true;
}

bool test_owned_real_factory_close_retains_and_collects_same_graph()
{
    FakeApi api;
    api.products_[0] = 0x0005U;
    api.products_[1] = 0x0005U;
    api.verify_wrapped_fds_closed = true;
    std::unique_ptr<asicen::LibusbContext> context_owner(
        new (std::nothrow) asicen::LibusbContext(api));
    PROOF_CHECK(context_owner != nullptr && context_owner->initialize(true) == 0 &&
                api.no_discovery);
    CallerPipe primary_fd;
    CallerPipe sibling_fd;
    PROOF_CHECK(primary_fd.read_fd() >= 0 && sibling_fd.read_fd() >= 0);

    GatedFakeUsb usb;
    usb.usb.terrestrial_b0 = 0xa9U;
    usb.usb.satellite_tsid_ready = true;
    const std::array<std::uint8_t, 16> recording_seed{{
        0x10U, 0x21U, 0x32U, 0x43U, 0x54U, 0x65U, 0x76U, 0x87U,
        0x98U, 0xa9U, 0xbaU, 0xcbU, 0xdcU, 0xedU, 0xfeU, 0x0fU}};
    usb.usb.seed_override = &recording_seed;
    std::unique_ptr<LibusbW3u3Hardware> hardware(new (std::nothrow)
        LibusbW3u3Hardware(context_owner->get(), primary_fd.read_fd(), sibling_fd.read_fd(),
                           nullptr, api));
    PROOF_CHECK(hardware != nullptr);
    usb.usb.hardware = hardware.get();
    Hooks hooks = usb.hooks();
    LibusbW3u3HardwareTestPeer::hooks_only(*hardware, &hooks);
    PROOF_CHECK(hardware->device_profile() != nullptr && hardware->claim());
    PROOF_CHECK(api.wrap_calls == 2 && api.wrapped_fds.size() == 2U);
    PROOF_CHECK(::fcntl(api.wrapped_fds[0], F_GETFD) >= 0 &&
                ::fcntl(api.wrapped_fds[1], F_GETFD) >= 0);

    const auto nonce = std::to_string(static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto directory = std::filesystem::temp_directory_path() /
                           ("asicen-owned-proof-" + nonce);
    PROOF_CHECK(std::filesystem::create_directory(directory));
    RuntimeDirectory cleanup_directory(directory);
    PROOF_CHECK(::chmod(directory.c_str(), 0700) == 0);
    const std::string instance = "owned-" + nonce.substr(
        nonce.size() > 8U ? nonce.size() - 8U : 0U);
    const EndpointConfig endpoint{directory.c_str(), instance.c_str(),
                                  kControlEndpointName, EndpointAccess::private_user};
    px4::userland::ipc::posix::EnclosurePublication publication;
    publication.runtime_directory = directory.string();
    publication.instance = instance;
    publication.lease_identity = instance;
    auto lease = SerialEndpointLease::acquire_identity(endpoint, instance);
    PROOF_CHECK(lease);
    auto created = asicen::create_live_card_stream_context(
        std::move(context_owner), std::move(hardware), std::move(lease.value()),
        publication, nullptr);
    PROOF_CHECK(created);
    std::unique_ptr<EnclosureContext> owner = std::move(created.value());

    factory_stop.store(false, std::memory_order_release);
    OwnedFactoryRunner runner(*owner);
    std::unique_ptr<ProofClient> client;
    PROOF_CHECK(wait_for(std::chrono::seconds(8), [&] {
        if (runner.done()) return true;
        client = ProofClient::connect(endpoint);
        return client != nullptr;
    }));
    PROOF_CHECK(client != nullptr && !runner.done());
    wire::HelloResponse hello;
    PROOF_CHECK(client->hello(hello));

    const char* recording_artifact = std::getenv("ASICEN_NORMAL_RECORDING_ARTIFACT");
    const auto recording_path = recording_artifact != nullptr
                                    ? std::filesystem::path(recording_artifact)
                                    : directory / "owned-normal.ts";
    const int recording_fd = ::open(recording_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                                    0600);
    PROOF_CHECK(recording_fd >= 0);
    px4::userland::cli::Px4TsArguments recording_arguments{};
    recording_arguments.valid = true;
    recording_arguments.instance = instance;
    recording_arguments.runtime_directory = directory.string();
    recording_arguments.receiver = 1U;
    recording_arguments.system = System::ISDB_T;
    recording_arguments.frequency_khz = 473142U;
    recording_arguments.tune_timeout_ms = 3000U;
    recording_arguments.packet_count_set = true;
    recording_arguments.packet_count = 1U;
    RecordingSignal recording_signal;
    std::atomic<bool> recording_done{false};
    std::atomic<int> recording_error{static_cast<int>(Error::INTERNAL)};
    const int capture_submits_before = usb.usb.submit_count.load();
    std::vector<std::uint8_t> expected_recording;
    std::thread recording_thread([&] {
        px4::userland::cli::Px4TsFdOutput output(recording_fd, true);
        RecordingClock clock;
        const auto result = output.ready()
            ? px4::userland::cli::Px4TsRunner::run_wire(
                  recording_arguments, output, clock, recording_signal)
            : Result<px4::userland::cli::Px4TsRunResult>::failure(Error::INTERNAL);
        recording_error.store(result ? 0 : static_cast<int>(result.error()),
                              std::memory_order_release);
        recording_done.store(true, std::memory_order_release);
    });
    const bool capture_submitted = wait_for(std::chrono::seconds(8), [&] {
        return usb.usb.submit_count.load() >= capture_submits_before + 4;
    });
    if (capture_submitted && !recording_done.load(std::memory_order_acquire)) {
        libusb_transfer* pending = usb.usb.submitted[0U];
        if (pending != nullptr && pending->callback != nullptr) {
            std::array<std::uint8_t, 188U * 5U> expected_packets{};
            for (std::size_t packet = 0U; packet < 5U; ++packet) {
                std::uint8_t* bytes = expected_packets.data() + packet * 188U;
                bytes[0U] = 0x47U;
                bytes[1U] = 0x00U;
                bytes[2U] = 0x20U;
                bytes[3U] = static_cast<std::uint8_t>(0x10U | packet);
            }
            transport_transform_reference::TransportMaterialV7 material{};
            PROOF_CHECK(transport_transform_reference::derive_transport_material_v7(
                recording_seed.data(), recording_seed.size(), &material));
            expected_recording.resize(188U);
            PROOF_CHECK(transport_transform_reference::transform_transport_packet_v7(
                expected_packets.data(), 188U, material.first_des_key.data(),
                material.second_des_key.data(), material.xor_state.data(),
                expected_recording.data()));
            volatile std::uint8_t* material_bytes =
                reinterpret_cast<volatile std::uint8_t*>(&material);
            for (std::size_t index = 0U; index < sizeof(material); ++index)
                material_bytes[index] = 0U;
            std::memcpy(pending->buffer, expected_packets.data(), expected_packets.size());
            pending->actual_length = static_cast<int>(expected_packets.size());
            pending->status = LIBUSB_TRANSFER_COMPLETED;
            pending->callback(pending);
            usb.usb.callback_count.fetch_add(1U);
            usb.usb.callback_total.fetch_add(1U);
            {
                std::lock_guard<std::mutex> lock(usb.usb.event_mutex);
                usb.usb.event_ready = true;
            }
            usb.usb.event_changed.notify_all();
        }
    } else {
        recording_signal.stopped.store(true, std::memory_order_release);
    }
    recording_thread.join();
    std::ifstream recording_input(recording_path, std::ios::binary);
    const std::vector<std::uint8_t> recorded{
        std::istreambuf_iterator<char>(recording_input), std::istreambuf_iterator<char>()};
    const char* expected_artifact = std::getenv("ASICEN_NORMAL_RECORDING_EXPECTED_ARTIFACT");
    if (expected_artifact != nullptr) {
        std::ofstream expected_output(expected_artifact, std::ios::binary | std::ios::trunc);
        expected_output.write(reinterpret_cast<const char*>(expected_recording.data()),
                              static_cast<std::streamsize>(expected_recording.size()));
        PROOF_CHECK(expected_output.good());
    }
    PROOF_CHECK(capture_submitted && recording_done.load(std::memory_order_acquire) &&
                recording_error.load(std::memory_order_acquire) == 0 &&
                recorded == expected_recording && usb.usb.dsc_start_count > 0 &&
                usb.usb.dsc_stop_count > 0 &&
                usb.usb.submit_count.load(std::memory_order_acquire) > 0);

    ProofClient::Reply acquire_reply;
    PROOF_CHECK(client->acquire(1U, acquire_reply));
    auto acquired = wire::decode_acquire_response(acquire_reply.view());
    PROOF_CHECK(acquired.has_value());
    ProofClient::Reply tune_reply;
    PROOF_CHECK(client->tune(acquired.value().lease_id, 473142857ULL,
                             wire::System::isdb_t, tune_reply));
    PROOF_CHECK(tune_reply.header.kind == wire::Kind::response);
    auto tuned = wire::decode_tune_result(tune_reply.view());
    PROOF_CHECK(tuned.has_value() && tuned.value().requested_hz == 473142857ULL);
    ProofClient::Reply release_reply;
    PROOF_CHECK(client->release(acquired.value().lease_id, release_reply));
    PROOF_CHECK(release_reply.header.kind == wire::Kind::response);
    ProofClient::Reply blocked_acquire_reply;
    PROOF_CHECK(client->acquire(1U, blocked_acquire_reply));
    auto blocked_acquired = wire::decode_acquire_response(blocked_acquire_reply.view());
    PROOF_CHECK(blocked_acquired.has_value());

    usb.block_next_control.store(true, std::memory_order_release);
    std::atomic<bool> tune_done{false};
    std::atomic<bool> tune_reply_received{false};
    std::thread tune_thread([&] {
        ProofClient::Reply reply;
        tune_reply_received.store(
            client->tune(blocked_acquired.value().lease_id, 473142857ULL,
                         wire::System::isdb_t, reply), std::memory_order_release);
        tune_done.store(true, std::memory_order_release);
    });
    GateThreadJoin join_tune(usb, tune_thread);
    const bool worker_entered = wait_for(std::chrono::seconds(3), [&] {
        return usb.control_entered.load(std::memory_order_acquire);
    });
    factory_stop.store(true, std::memory_order_release);
    const auto timed_out = owner->close(std::chrono::milliseconds{80});
    const auto competing_lease = SerialEndpointLease::acquire_identity(endpoint, instance);
    const bool lease_retained = !competing_lease && competing_lease.error() == Error::BUSY;
    const bool fds_retained = api.wrapped_fds.size() == 2U &&
        ::fcntl(api.wrapped_fds[0], F_GETFD) >= 0 &&
        ::fcntl(api.wrapped_fds[1], F_GETFD) >= 0;
    const bool operation_still_blocked = !tune_done.load(std::memory_order_acquire);
    usb.release();
    join_tune.join();
    const auto collected = owner->close(std::chrono::seconds{5});
    const bool runner_joined = runner.stop_and_join();
    const bool no_repeat_shutdown = !runner.deadline_expired();
    const int runner_result = runner.result();
    client.reset();
    owner.reset();

    const bool descriptors_closed = api.wrapped_fds.size() == 2U &&
        ::fcntl(api.wrapped_fds[0], F_GETFD) == -1 &&
        ::fcntl(api.wrapped_fds[1], F_GETFD) == -1;
    const auto released_lease = SerialEndpointLease::acquire_identity(endpoint, instance);
    const bool lease_released = released_lease.has_value();
    PROOF_CHECK(worker_entered && operation_still_blocked && !timed_out.quiesced &&
                timed_out.deadline_expired && lease_retained && fds_retained &&
                collected.quiesced && runner_joined && no_repeat_shutdown &&
                runner_result == 0 && descriptors_closed && lease_released &&
                ::fcntl(primary_fd.read_fd(), F_GETFD) >= 0 &&
                ::fcntl(sibling_fd.read_fd(), F_GETFD) >= 0 && api.valid &&
                cleanup_order(api, {"release2", "release1", "close1", "close2", "exit"}));
    (void)tune_reply_received;
    return true;
}

}  // namespace

int main()
{
    return test_actual_s3u_and_w3u3_live_factories() &&
                   test_owned_real_factory_close_retains_and_collects_same_graph()
               ? 0 : 1;
}
