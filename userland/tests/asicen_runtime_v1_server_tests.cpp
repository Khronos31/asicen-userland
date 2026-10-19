// SPDX-License-Identifier: GPL-2.0-only
//
// UDTV control endpoint of the actual product server: real AF_UNIX sockets,
// the real worker lanes and the same TunerService/receiver engine, with mock
// receiver and card backends only.  The small client below is test code that
// uses the common codecs; it is not a shipping client.
#include "control_server_test_access.h"
#include "control_workers.h"
#include "px4/card_service.h"
#include "asicen/control_context_builder.h"
#include "asicen/device_profile.h"
#include "asicen/hardware_stream_session.h"
#include "asicen/px4_mock_backend.h"
#include "px4/control_server.h"
#include "px4/posix_tuner_nonce.h"
#include "px4/tuner_service.h"
#include "px4_ts_core.h"
#include "px4_ts_posix.h"

#include "dtv/userland/control_connection.h"
#include "dtv/userland/json.h"
#include "dtv/userland/records.h"
#include "dtv/userland/wire.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;
using asicen::ClaimedUsbFunctionObservation;
namespace wire = dtv::userland::wire;

#define V1_CHECK(condition)                                                           \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "runtime v1 check failed at %d: %s\n", __LINE__,     \
                         #condition);                                                  \
            return false;                                                              \
        }                                                                              \
    } while (false)

constexpr std::chrono::milliseconds kWaitBound{5000};

template <typename T>
bool wait_for(std::chrono::milliseconds bound, T predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// ---------------------------------------------------------------------------
// Backends

class Gate final {
public:
    void arm()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        armed_ = true;
        entered_ = false;
        open_ = false;
    }
    void pass()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!armed_) return;
        armed_ = false;
        entered_ = true;
        changed_.notify_all();
        changed_.wait_for(lock, std::chrono::seconds(10), [&]() noexcept { return open_; });
    }
    bool wait_entered()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, kWaitBound, [&]() noexcept { return entered_; });
    }
    void open()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        armed_ = false;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool armed_ = false;
    bool entered_ = false;
    bool open_ = false;
};

struct RequestStopHooks final {
    std::atomic<std::size_t> calls{0U};
    Gate* release_gate = nullptr;
    void notify() noexcept
    {
        if (calls.fetch_add(1U) + 1U == 4U && release_gate != nullptr)
            release_gate->open();
    }
};

// Counts every card-side call: generic STATUS must never reach any of them.
class CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override
    {
        power_calls.fetch_add(1U);
        return Result<void>::success();
    }
    Result<void> initialize_uart() noexcept override
    {
        uart_calls.fetch_add(1U);
        return Result<void>::success();
    }
    Result<bool> detect_card() noexcept override
    {
        detect_calls.fetch_add(1U);
        return Result<bool>::success(true);
    }
    void request_stop() noexcept override
    {
        stop_calls.fetch_add(1U);
        if (stop_hooks != nullptr) stop_hooks->notify();
    }
    RequestStopHooks* stop_hooks = nullptr;
    std::atomic<std::size_t> power_calls{0U};
    std::atomic<std::size_t> uart_calls{0U};
    std::atomic<std::size_t> detect_calls{0U};
    std::atomic<std::size_t> stop_calls{0U};
};

class CardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override
    {
        initialize_calls.fetch_add(1U);
        return Result<void>::success();
    }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    {
        transmit_calls.fetch_add(1U);
        return Result<std::size_t>::failure(Error::UNSUPPORTED);
    }
    bool initialized() const noexcept override { return false; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}
    void request_stop() noexcept override
    {
        stop_calls.fetch_add(1U);
        if (stop_hooks != nullptr) stop_hooks->notify();
    }
    RequestStopHooks* stop_hooks = nullptr;
    std::atomic<std::size_t> initialize_calls{0U};
    std::atomic<std::size_t> transmit_calls{0U};
    std::atomic<std::size_t> stop_calls{0U};

private:
    CardAtr atr_{};
};

class StreamControl final : public TunerStreamControl {
public:
    void request_stop() noexcept override
    {
        stop_calls.fetch_add(1U);
        if (stop_hooks != nullptr) stop_hooks->notify();
    }
    Result<void> attach(const TunerAttachment&) noexcept override
    { return Result<void>::failure(Error::UNSUPPORTED); }
    Result<void> detach(const TunerAttachment&) noexcept override
    { return Result<void>::failure(Error::UNSUPPORTED); }
    std::atomic<std::size_t> stop_calls{0U};
    RequestStopHooks* stop_hooks = nullptr;
};

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, kNonceLength> value{};
        value.fill(0x5aU);
        value[0U] = next_.fetch_add(1U);
        return Result<std::array<std::uint8_t, kNonceLength>>::success(value);
    }

private:
    std::atomic<std::uint8_t> next_{1U};
};

class LeaseIds final : public TunerLeaseIdSource {
public:
    Result<std::uint64_t> next() noexcept override
    {
        return Result<std::uint64_t>::success(0x91b4c2d87730e5a6ULL + next_++);
    }

private:
    std::uint64_t next_ = 0U;
};

// Manually advanced engine clock; sleeps advance it, so lock polls finish.
class Clock final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return now.load(); }
    void sleep_ms(std::uint32_t milliseconds) noexcept override { now.fetch_add(milliseconds); }
    std::atomic<std::uint64_t> now{1000U};
};

class RecordingSource final : public asicen::StreamCaptureSource {
public:
    Result<void> prepare(std::uint8_t receiver, System system,
                         const std::atomic<bool>& cancelled) noexcept override
    {
        return receiver == 1U && system == System::ISDB_T && !cancelled.load()
                   ? Result<void>::success()
                   : Result<void>::failure(Error::UNSUPPORTED);
    }
    asicen::CaptureRunResult run(
        const std::atomic<bool>& cancelled,
        bool (*emit)(void*, const std::uint8_t*, std::size_t), void* context) noexcept override
    {
        if (!packets.empty() && !emit(context, packets.data(), packets.size()))
            return asicen::CaptureRunResult::usb_error;
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return cancelled.load() || interrupted; });
        return asicen::CaptureRunResult::cancelled;
    }
    void interrupt() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            interrupted = true;
        }
        changed.notify_all();
    }
    Result<void> stop() noexcept override { return Result<void>::success(); }

    std::vector<std::uint8_t> packets;

private:
    std::mutex mutex;
    std::condition_variable changed;
    bool interrupted = false;
};

// PX-Q3U4 receiver layout (0/1/4/5 ISDB-S, 2/3/6/7 ISDB-T) with the reviewed
// native strict profiles, so TUNE reaches the strict Hz entrance.
class Backend final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override { return 2U; }
    bool receiver_supports(std::uint8_t receiver, System system) const noexcept override
    {
        return receiver == 0U ? system == System::ISDB_S
                              : receiver == 1U && system == System::ISDB_T;
    }
    TunerStrictProfile strict_profile(std::uint8_t receiver,
                                      System system) const noexcept override
    {
        if (!receiver_supports(receiver, system)) return {};
        TunerStrictProfile profile{};
        profile.native_hz = true;
        if (system == System::ISDB_T) {
            profile.min_hz = 40000000U;
            profile.max_hz = 889999999U;
        } else {
            profile.min_hz = 1049480000U;
            profile.max_hz = 2053000000U;
            profile.rows[0] = TunerFrequencyRows{1049480000U, 38360000U, 12U};
            profile.rows[1] = TunerFrequencyRows{1613000000U, 40000000U, 12U};
            profile.row_sets = 2U;
            profile.lnb_voltage_mask = 0x01U;
            profile.slot_count = 8U;
        }
        return profile;
    }
    bool requires_terrestrial_lock_settle() const noexcept override { return false; }
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        open_entered[receiver].store(true);
        if (receiver == gated_open_receiver.load()) {
            open_gate.pass();
            open_returned_after_stop.store(stopping.load());
        }
        open_calls[receiver].fetch_add(1U);
        return Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    {
        khz_calls.fetch_add(1U);
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    {
        khz_calls.fetch_add(1U);
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<TunerHzProgramming> tune_terrestrial_hz(std::uint8_t receiver, std::uint64_t hz,
                                                   std::uint32_t) noexcept override
    {
        return program(receiver, hz);
    }
    Result<TunerHzProgramming> tune_satellite_hz(std::uint8_t receiver, std::uint64_t hz,
                                                 std::uint32_t) noexcept override
    {
        return program(receiver, hz);
    }
    Result<bool> is_locked(std::uint8_t, System) noexcept override
    {
        lock_calls.fetch_add(1U);
        return Result<bool>::success(true);
    }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    {
        if (receiver == gated_close_receiver.load()) close_gate.pass();
        close_calls[receiver].fetch_add(1U);
        return close_error.load() == Error::OK ? Result<void>::success()
                                               : Result<void>::failure(close_error.load());
    }
    void request_stop() noexcept override
    {
        stopping.store(true);
        if (stop_hooks != nullptr) stop_hooks->notify();
    }
    Result<void> begin_tune_power(std::uint8_t receiver, System,
                                  std::uint8_t voltage) noexcept override
    {
        power_calls[receiver].fetch_add(1U);
        last_voltage.store(voltage);
        return Result<void>::success();
    }
    Result<void> start_capture(std::uint8_t, System) noexcept override
    {
        capture_calls.fetch_add(1U);
        return Result<void>::success();
    }
    Result<void> stop_capture(std::uint8_t, System) noexcept override
    {
        return Result<void>::success();
    }

    Result<TunerHzProgramming> program(std::uint8_t receiver, std::uint64_t hz) noexcept
    {
        if (receiver == gated_tune_receiver.load()) tune_gate.pass();
        hz_calls[receiver].fetch_add(1U);
        last_hz[receiver].store(hz);
        TunerHzProgramming programming{};
        // A deterministic driver command value distinct from the request.
        programming.programmed_hz.present = true;
        programming.programmed_hz.value = (hz / 1000U) * 1000U;
        return Result<TunerHzProgramming>::success(programming);
    }

    std::array<std::atomic<std::size_t>, kReceiverCount> open_calls{};
    std::array<std::atomic<std::size_t>, kReceiverCount> close_calls{};
    std::array<std::atomic<std::size_t>, kReceiverCount> hz_calls{};
    std::array<std::atomic<std::size_t>, kReceiverCount> power_calls{};
    std::array<std::atomic<bool>, kReceiverCount> open_entered{};
    std::array<std::atomic<std::uint64_t>, kReceiverCount> last_hz{};
    std::atomic<std::size_t> khz_calls{0U};
    std::atomic<std::size_t> lock_calls{0U};
    std::atomic<std::size_t> capture_calls{0U};
    std::atomic<std::uint8_t> last_voltage{0xffU};
    std::atomic<Error> close_error{Error::OK};
    std::atomic<std::uint8_t> gated_tune_receiver{0xffU};
    std::atomic<std::uint8_t> gated_close_receiver{0xffU};
    Gate tune_gate;
    Gate close_gate;
    Gate open_gate;
    RequestStopHooks* stop_hooks = nullptr;
    std::atomic<std::uint8_t> gated_open_receiver{0xffU};
    std::atomic<bool> stopping{false};
    std::atomic<bool> open_returned_after_stop{false};
};

std::size_t total(const std::array<std::atomic<std::size_t>, kReceiverCount>& counts)
{
    std::size_t sum = 0U;
    for (const auto& count : counts) sum += count.load();
    return sum;
}

// ---------------------------------------------------------------------------
// Context

class TempRuntime final {
public:
    TempRuntime()
    {
        const std::string base = std::filesystem::temp_directory_path().string();
        std::string pattern = base + "/px4-v1-XXXXXX";
        char* made = ::mkdtemp(pattern.data());
        if (made != nullptr) {
            path_ = made;
            valid_ = ::chmod(path_.c_str(), 0700) == 0;
        }
    }
    ~TempRuntime() noexcept
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    bool valid() const noexcept { return valid_; }
    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    bool valid_ = false;
};

wire::Uuid make_epoch(std::uint8_t seed)
{
    wire::Uuid epoch{};
    for (std::size_t i = 0U; i < epoch.size(); ++i)
        epoch[i] = static_cast<std::uint8_t>(seed + i * 7U);
    return epoch;
}

ClaimedUsbFunctionObservation bridge(std::uint8_t dev, std::uint8_t address)
{
    ClaimedUsbFunctionObservation observation;
    observation.vendor_id = 0x0b06U;
    observation.product_id = 0x0005U;
    observation.has_bus = true;
    observation.bus = 3U;
    observation.has_address = true;
    observation.address = address;
    observation.port_path = {1U, dev};
    return observation;
}

// The product builder over a PX-Q3U4-shaped service and two observed bridges.
bool product_context(const TunerService& tuner, const wire::Uuid& epoch,
                     ControlContext& context, ClaimedUsbFunctionObservation& first,
                     ClaimedUsbFunctionObservation& second,
                     const char* instance = "w3u3-test")
{
    first = bridge(1U, 11U);
    second = bridge(2U, 12U);
    const auto* profile = asicen::find_profile(asicen::ModelId::W3u3);
    return profile != nullptr && asicen::build_asicen_control_context(
        *profile, std::array<ClaimedUsbFunctionObservation, 2U>{first, second}.data(),
        2U, 2U, tuner, instance, epoch, context);
}

bool test_asicen_context_profile_facts()
{
    Nonce nonce;
    Clock clock;
    const wire::Uuid epoch = make_epoch(0x42U);
    const auto* combined = asicen::find_profile(asicen::ModelId::S3u);
    const auto* paired = asicen::find_profile(asicen::ModelId::W3u3);
    V1_CHECK(combined != nullptr && paired != nullptr);

    asicen::MockTunerBackend combined_backend(1U);
    TunerService combined_tuner(combined_backend, nonce, clock);
    std::array<ClaimedUsbFunctionObservation, 2U> combined_observation{};
    combined_observation[0].vendor_id = combined->vid;
    combined_observation[0].product_id = combined->pid;
    ControlContext combined_context;
    V1_CHECK(asicen::build_asicen_control_context(*combined,
        combined_observation.data(), 1U, 1U, combined_tuner, "combined",
        epoch, combined_context));
    const wire::ListResponse& one = combined_context.list;
    V1_CHECK(wire::validate_list_response(one) == dtv::userland::Error::OK &&
             one.functions.count == 1U && one.topology.expected_function_count.value == 1U &&
             one.receivers.value.count == 1U &&
             one.receivers.value.items[0].systems.mask == 0x03U);

    asicen::MockTunerBackend paired_backend(2U);
    TunerService paired_tuner(paired_backend, nonce, clock);
    std::array<ClaimedUsbFunctionObservation, 2U> paired_observations{};
    for (std::size_t i = 0U; i < paired_observations.size(); ++i) {
        paired_observations[i].vendor_id = paired->vid;
        paired_observations[i].product_id = paired->pid;
    }
    paired_observations[0].has_bus = true;
    paired_observations[0].bus = 4U;
    paired_observations[0].port_path = {2U, 1U};
    paired_observations[1].has_bus = true;
    paired_observations[1].bus = 4U;
    paired_observations[1].port_path = {2U, 2U};
    ControlContext paired_context;
    V1_CHECK(asicen::build_asicen_control_context(*paired,
        paired_observations.data(), 2U, 2U, paired_tuner, "paired",
        epoch, paired_context));
    const wire::ListResponse& two = paired_context.list;
    V1_CHECK(wire::validate_list_response(two) == dtv::userland::Error::OK &&
             two.functions.count == 2U && two.topology.expected_function_count.value == 2U &&
             two.receivers.value.count == 2U && two.readers.value.count == 1U);
    V1_CHECK(two.functions.items[1].location.usb_path.present &&
        std::string(two.functions.items[1].location.usb_path.value.bytes.data(),
                    two.functions.items[1].location.usb_path.value.length) == "4-2.2");
    V1_CHECK(two.enclosure.value.identity.serial_state == wire::IdentitySerialState::absent &&
             paired->enclosure_receiver_count == 4U &&
             two.enclosure.value.limits.max_concurrent_streams.present &&
             two.enclosure.value.limits.max_concurrent_streams.value == 1U &&
             two.functions.items[1].serial_state == wire::SerialState::absent &&
             two.receivers.value.items[0].binding.value.function_id == 0U &&
             two.receivers.value.items[1].binding.value.function_id == 0U);
    return true;
}

// Synthetic dispatcher coverage: sparse public IDs including 0 over legacy
// receivers 0, 1 and 2, and a known-empty reader set.  Not a P topology.
void sparse_context(ControlContext& context, const wire::Uuid& epoch)
{
    context = ControlContext{};
    context.epoch = epoch;
    wire::ListResponse& list = context.list;
    list.epoch = epoch;
    const char product[] = "synthetic";
    list.producer.product.length = sizeof(product) - 1U;
    std::memcpy(list.producer.product.bytes.data(), product, sizeof(product) - 1U);
    list.producer.version.length = 1U;
    list.producer.version.bytes[0] = '1';
    list.topology.state = wire::TopologyState::complete;
    list.functions.count = 1U;
    list.functions.items[0].id = 0U;
    list.functions.items[0].vid = 0x1234U;
    list.functions.items[0].pid = 0x5678U;
    list.functions.items[0].mode = wire::Mode::runtime;
    list.functions.items[0].serial_state = wire::SerialState::absent;
    list.functions.items[0].identification_reason = wire::IdentificationReason::model_unconfirmed;
    list.functions.items[0].present = true;
    list.receivers.present = true;
    list.receivers.value.count = 3U;
    const std::uint16_t ids[3] = {0U, 7U, 513U};
    const std::uint8_t legacy[3] = {2U, 3U, 6U};
    for (std::size_t i = 0U; i < 3U; ++i) {
        list.receivers.value.items[i].id = ids[i];
        list.receivers.value.items[i].systems.mask = 0x01U;
        list.receivers.value.items[i].binding.present = true;
        list.receivers.value.items[i].binding.value =
            wire::Binding{0U, static_cast<std::uint16_t>(i)};
        context.receiver_index[i] = legacy[i];
    }
    list.readers.present = true;
    list.readers.value.count = 0U;
}

// ---------------------------------------------------------------------------
// Test client: one UDTV control connection, one request at a time.

struct Reply final {
    wire::Header header{};
    std::vector<std::uint8_t> payload;
    ByteView view() const { return ByteView{payload.data(), payload.size()}; }
};

class Collector final : public wire::FrameHandler {
public:
    Result<void> on_frame(const wire::Frame& frame) noexcept override
    {
        Reply reply;
        reply.header = frame.header;
        reply.payload.assign(frame.payload.data, frame.payload.data + frame.payload.size);
        frames.push_back(std::move(reply));
        return Result<void>::success();
    }
    std::vector<Reply> frames;
};

class TestClient final {
public:
    explicit TestClient(SocketStream stream)
        : stream_(std::move(stream)),
          storage_(new std::array<std::uint8_t, wire::kMaxControlPayload>()),
          receiver_(MutableByteView{storage_->data(), storage_->size()})
    {
    }

    static std::unique_ptr<TestClient> connect(const EndpointConfig& endpoint)
    {
        auto stream = SocketStream::connect(endpoint, Timeout{2000U});
        if (!stream) return nullptr;
        return std::unique_ptr<TestClient>(new TestClient(std::move(stream.value())));
    }

    bool send_bytes(const std::uint8_t* data, std::size_t size)
    {
        std::size_t sent = 0U;
        while (sent < size) {
            const ssize_t result = ::send(stream_.native_handle(), data + sent,
                                          size - sent, MSG_NOSIGNAL);
            if (result <= 0) return false;
            sent += static_cast<std::size_t>(result);
        }
        return true;
    }

    // Encodes one frame into bytes without sending it.
    static std::vector<std::uint8_t> frame(std::uint16_t opcode, std::uint32_t request_id,
                                           ByteView payload,
                                           wire::Kind kind = wire::Kind::request)
    {
        std::vector<std::uint8_t> bytes(wire::kHeaderSize + payload.size);
        wire::Header header;
        header.kind = kind;
        header.opcode = opcode;
        header.request_id = request_id;
        header.payload_length = static_cast<std::uint32_t>(payload.size);
        const auto written = wire::encode_frame(header, payload,
                                                MutableByteView{bytes.data(), bytes.size()});
        if (!written) bytes.clear();
        return bytes;
    }

    template <typename Record>
    static std::vector<std::uint8_t> encode(const Record& value,
                                            Result<std::size_t> (*encoder)(const Record&,
                                                                           MutableByteView) noexcept)
    {
        std::vector<std::uint8_t> bytes(wire::kMaxControlPayload);
        const auto size = encoder(value, MutableByteView{bytes.data(), bytes.size()});
        bytes.resize(size ? size.value() : 0U);
        return bytes;
    }

    // Reads until one frame arrives, the peer closes, or the bound expires.
    bool read_frame(Reply& out, std::chrono::milliseconds bound = kWaitBound)
    {
        const auto deadline = std::chrono::steady_clock::now() + bound;
        while (collector_.frames.empty()) {
            if (std::chrono::steady_clock::now() >= deadline || closed_) return false;
            std::array<std::uint8_t, 4096U> buffer{};
            const auto read = stream_.read_some(MutableByteView{buffer.data(), buffer.size()},
                                                Timeout{20U});
            if (!read) {
                if (read.error() == Error::DISCONNECTED) closed_ = true;
                continue;
            }
            if (!receiver_.feed(ByteView{buffer.data(), read.value()}, collector_)) return false;
        }
        out = std::move(collector_.frames.front());
        collector_.frames.erase(collector_.frames.begin());
        return true;
    }

    // True when the server closed this connection without sending a frame.
    bool closed_without_reply(std::chrono::milliseconds bound = kWaitBound)
    {
        Reply reply;
        if (read_frame(reply, bound)) return false;
        return closed_;
    }

    bool request(std::uint16_t opcode, const std::vector<std::uint8_t>& payload, Reply& out)
    {
        const auto bytes = frame(opcode, ++request_id_, ByteView{payload.data(), payload.size()});
        if (bytes.empty() || !send_bytes(bytes.data(), bytes.size())) return false;
        return read_frame(out) && out.header.request_id == request_id_ &&
               out.header.opcode == opcode;
    }

    bool hello(wire::HelloResponse* response = nullptr, std::uint64_t required = 0U)
    {
        wire::HelloRequest hello;
        hello.min = wire::Version{1U, 0U};
        hello.max = wire::Version{1U, 0U};
        hello.required_features = required;
        Reply reply;
        if (!request(wire::kOpcodeHello, encode(hello, &wire::encode_hello_request), reply) ||
            reply.header.kind != wire::Kind::response)
            return false;
        const auto decoded = wire::decode_hello_response(reply.view());
        if (!decoded) return false;
        epoch = decoded.value().epoch;
        if (response != nullptr) *response = decoded.value();
        return true;
    }

    static std::uint32_t error_code(const Reply& reply)
    {
        if (reply.header.kind != wire::Kind::error_response) return 0U;
        const auto decoded = wire::decode_error_response(reply.view());
        return decoded ? decoded.value().error.code : 0U;
    }

    bool list(wire::ListResponse& out)
    {
        wire::ListRequest request_value;
        request_value.epoch = epoch;
        Reply reply;
        if (!request(wire::kOpcodeList, encode(request_value, &wire::encode_list_request), reply) ||
            reply.header.kind != wire::Kind::response)
            return false;
        auto decoded = wire::decode_list_response(reply.view());
        if (!decoded) return false;
        out = decoded.value();
        return true;
    }

    // STATUS reply; status is decoded only for a success reply.
    bool status(const wire::Scope& scope, Reply& reply, wire::StatusResponse* status = nullptr,
                const wire::Uuid* use_epoch = nullptr)
    {
        wire::StatusRequest request_value;
        request_value.epoch = use_epoch != nullptr ? *use_epoch : epoch;
        request_value.scope = scope;
        if (!request(wire::kOpcodeStatus, encode(request_value, &wire::encode_status_request),
                     reply))
            return false;
        if (status != nullptr && reply.header.kind == wire::Kind::response) {
            auto decoded = wire::decode_status_response(reply.view());
            if (!decoded) return false;
            *status = decoded.value();
        }
        return true;
    }

    bool acquire(std::uint16_t receiver, Reply& reply, const wire::Uuid* use_epoch = nullptr)
    {
        wire::AcquireRequest request_value;
        request_value.epoch = use_epoch != nullptr ? *use_epoch : epoch;
        request_value.receiver_id = receiver;
        return request(wire::kOpcodeAcquire,
                       encode(request_value, &wire::encode_acquire_request), reply);
    }

    std::uint64_t acquire_lease(std::uint16_t receiver)
    {
        Reply reply;
        if (!acquire(receiver, reply) || reply.header.kind != wire::Kind::response) return 0U;
        const auto decoded = wire::decode_acquire_response(reply.view());
        return decoded && decoded.value().epoch == epoch ? decoded.value().lease_id : 0U;
    }

    bool release(std::uint64_t lease, Reply& reply)
    {
        wire::ReleaseRequest request_value;
        request_value.epoch = epoch;
        request_value.lease_id = lease;
        return request(wire::kOpcodeRelease,
                       encode(request_value, &wire::encode_release_request), reply);
    }

    bool tune(const wire::TuneRequest& value, Reply& reply)
    {
        return request(wire::kOpcodeTune, encode(value, &wire::encode_tune_request), reply);
    }

    // Sends a request without waiting for its reply.
    bool send_request(std::uint16_t opcode, const std::vector<std::uint8_t>& payload)
    {
        const auto bytes = frame(opcode, ++request_id_, ByteView{payload.data(), payload.size()});
        return !bytes.empty() && send_bytes(bytes.data(), bytes.size());
    }

    wire::TuneRequest terrestrial(std::uint64_t lease, std::uint64_t hz,
                                  std::uint32_t timeout_ms = 1000U) const
    {
        wire::TuneRequest value;
        value.epoch = epoch;
        value.lease_id = lease;
        value.system = wire::System::isdb_t;
        value.frequency_hz = hz;
        value.timeout_ms = timeout_ms;
        return value;
    }

    void close() { stream_.close(); }
    int fd() const { return stream_.native_handle(); }
    std::uint32_t last_request_id() const { return request_id_; }

    wire::Uuid epoch{};

private:
    SocketStream stream_;
    std::unique_ptr<std::array<std::uint8_t, wire::kMaxControlPayload>> storage_;
    wire::FrameReceiver receiver_;
    Collector collector_;
    std::uint32_t request_id_ = 0U;
    bool closed_ = false;
};

// ---------------------------------------------------------------------------
// Server fixture

struct Fixture final {
    TempRuntime runtime;
    std::string instance;
    EndpointConfig endpoint{};
    EndpointConfig stream_endpoint{};
    CardBackend card_backend;
    CardSession card_session;
    StreamControl stream_control;
    RecordingSource recording_source;
    std::unique_ptr<asicen::HardwareStreamService> recording_stream;
    std::unique_ptr<CardService> card;
    Backend backend;
    RequestStopHooks stop_hooks;
    Nonce nonce;
    LeaseIds lease_ids;
    Clock clock;
    std::unique_ptr<TunerService> tuner;
    std::unique_ptr<ControlContext> context{new ControlContext()};
    ClaimedUsbFunctionObservation first{};
    ClaimedUsbFunctionObservation second{};
    std::unique_ptr<PosixControlServer> server;
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread poller;

    explicit Fixture(const char* name, bool normal_recording = false) : instance(name)
    {
        endpoint = EndpointConfig{runtime.path().c_str(), instance.c_str(),
                                  kControlEndpointName, EndpointAccess::private_user};
        stream_endpoint = EndpointConfig{runtime.path().c_str(), instance.c_str(),
                                         kStreamEndpointName, EndpointAccess::private_user};
        card.reset(new CardService(card_backend, card_session));
        stop_hooks.release_gate = &backend.open_gate;
        backend.stop_hooks = &stop_hooks;
        card_backend.stop_hooks = &stop_hooks;
        card_session.stop_hooks = &stop_hooks;
        stream_control.stop_hooks = &stop_hooks;
        if (normal_recording) {
            recording_stream.reset(new asicen::HardwareStreamService(
                backend, recording_source,
                asicen::HardwareStreamService::TestQueueCapacityBytes{64U * 188U}));
        }
        TunerServiceBackend& tuner_backend = recording_stream
            ? static_cast<TunerServiceBackend&>(*recording_stream)
            : static_cast<TunerServiceBackend&>(backend);
        TunerStreamControl* tuner_stream = recording_stream
            ? static_cast<TunerStreamControl*>(recording_stream.get())
            : static_cast<TunerStreamControl*>(&stream_control);
        tuner.reset(new TunerService(tuner_backend, nonce, clock, &lease_ids, nullptr,
                                     tuner_stream));
    }

    bool start(bool sparse = false, std::uint8_t epoch_seed = 0x10U,
               const char* product_instance = "w3u3-test")
    {
        if (!runtime.valid() || !tuner->configure_strict_capabilities()) return false;
        if (sparse) {
            sparse_context(*context, make_epoch(epoch_seed));
        } else if (!product_context(*tuner, make_epoch(epoch_seed), *context, first, second,
                                    product_instance)) {
            return false;
        }
        TunerStreamControl* stream = recording_stream
            ? static_cast<TunerStreamControl*>(recording_stream.get())
            : static_cast<TunerStreamControl*>(&stream_control);
        auto created = PosixControlServer::create(endpoint, *card, *tuner, *context, stream);
        if (!created) {
            std::fprintf(stderr, "server create failed: %s\n", error_string(created.error()));
            return false;
        }
        server = std::move(created.value());
        poller = std::thread([this]() {
            while (!stop.load()) {
                if (!server->poll_once(Timeout{10U})) {
                    server_ok.store(false);
                    return;
                }
            }
        });
        return true;
    }

    ~Fixture()
    {
        backend.tune_gate.open();
        backend.close_gate.open();
        stop.store(true);
        if (poller.joinable()) poller.join();
        if (server) (void)server->shutdown();
        if (card) (void)card->shutdown();
    }
};

class RecordingClientClock final : public px4::userland::cli::Px4TsClock {
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

class RecordingClientSignal final : public px4::userland::cli::Px4TsSignal {
public:
    bool stop_requested() const noexcept override { return false; }
};

std::vector<std::uint8_t> recording_packets(std::size_t count)
{
    std::vector<std::uint8_t> result(count * 188U);
    for (std::size_t index = 0U; index < count; ++index) {
        std::uint8_t* packet = result.data() + index * 188U;
        packet[0U] = 0x47U;
        packet[1U] = 0x00U;
        packet[2U] = 0x20U;
        packet[3U] = static_cast<std::uint8_t>(0x10U | (index & 0x0fU));
        for (std::size_t offset = 4U; offset < 188U; ++offset)
            packet[offset] = static_cast<std::uint8_t>((index * 29U + offset * 7U) & 0xffU);
    }
    return result;
}

std::vector<std::uint8_t> read_recording_file(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

bool test_normal_recording_file_output()
{
    Fixture fixture("normal-recording", true);
    fixture.recording_source.packets = recording_packets(32U);
    V1_CHECK(fixture.start());
    const std::string path = fixture.runtime.path() + "/normal.ts";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    V1_CHECK(fd >= 0);

    px4::userland::cli::Px4TsArguments arguments{};
    arguments.valid = true;
    arguments.instance = fixture.instance;
    arguments.runtime_directory = fixture.runtime.path();
    arguments.receiver = 1U;
    arguments.system = System::ISDB_T;
    arguments.frequency_khz = 473142U;
    arguments.tune_timeout_ms = 3000U;
    arguments.output = "-";
    arguments.packet_count_set = true;
    arguments.packet_count = 32U;

    px4::userland::cli::Px4TsFailureKind failure =
        px4::userland::cli::Px4TsFailureKind::ipc;
    px4::userland::cli::Px4TsRunDiagnostics diagnostics{};
    px4::userland::cli::Px4TsRunResult run_result{};
    {
        px4::userland::cli::Px4TsFdOutput output(fd, true);
        V1_CHECK(output.ready());
        RecordingClientClock clock;
        RecordingClientSignal signal;
        const auto result = px4::userland::cli::Px4TsRunner::run_wire(
            arguments, output, clock, signal, &failure, &diagnostics);
        if (!result) std::fprintf(stderr, "recording runner: %s kind=%u\n",
                                  error_string(result.error()),
                                  static_cast<unsigned>(failure));
        V1_CHECK(result);
        run_result = result.value();
    }
    const std::vector<std::uint8_t> written = read_recording_file(path);
    const std::vector<std::uint8_t> expected = recording_packets(32U);
    V1_CHECK(written == expected);
    V1_CHECK(diagnostics.stream_end_valid && diagnostics.packets_written == 32U);
    V1_CHECK(run_result.counters.packets >= 32U && run_result.counters.queue_drops == 0U);
    V1_CHECK(fixture.server_ok.load());
    return true;
}

bool test_normal_recording_duration_without_ts()
{
    Fixture fixture("normal-duration", true);
    V1_CHECK(fixture.start());
    const std::string path = fixture.runtime.path() + "/duration.ts";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    V1_CHECK(fd >= 0);

    px4::userland::cli::Px4TsArguments arguments{};
    arguments.valid = true;
    arguments.instance = fixture.instance;
    arguments.runtime_directory = fixture.runtime.path();
    arguments.receiver = 1U;
    arguments.system = System::ISDB_T;
    arguments.frequency_khz = 473142U;
    arguments.tune_timeout_ms = 3000U;
    arguments.output = "-";
    arguments.duration_set = true;
    arguments.duration_seconds = 1U;

    px4::userland::cli::Px4TsFailureKind failure =
        px4::userland::cli::Px4TsFailureKind::ipc;
    px4::userland::cli::Px4TsRunDiagnostics diagnostics{};
    {
        px4::userland::cli::Px4TsFdOutput output(fd, true);
        V1_CHECK(output.ready());
        RecordingClientClock clock;
        RecordingClientSignal signal;
        const auto result = px4::userland::cli::Px4TsRunner::run_wire(
            arguments, output, clock, signal, &failure, &diagnostics);
        if (!result) std::fprintf(stderr, "duration runner: %s kind=%u\n",
                                  error_string(result.error()),
                                  static_cast<unsigned>(failure));
        V1_CHECK(result);
    }
    V1_CHECK(diagnostics.stream_end_valid && diagnostics.packets_written == 0U);
    V1_CHECK(diagnostics.stream_end.counters.packets == 0U);
    V1_CHECK(read_recording_file(path).empty());
    V1_CHECK(fixture.server_ok.load());
    return true;
}

bool product_recording_case(const char* name, const char* device, bool include_instance,
                            bool expect_success, Error expected_error)
{
    Fixture fixture(name, true);
    fixture.recording_source.packets = recording_packets(32U);
    V1_CHECK(fixture.start(false, 0x10U, fixture.instance.c_str()));
    const std::string path = fixture.runtime.path() + "/product.ts";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    V1_CHECK(fd >= 0);

    px4::userland::cli::Px4TsArguments arguments{};
    arguments.valid = true;
    arguments.product_cli = true;
    if (device != nullptr) arguments.device = device;
    if (include_instance) arguments.instance = fixture.instance;
    arguments.runtime_directory = fixture.runtime.path();
    arguments.receiver = 1U;
    arguments.system = System::ISDB_T;
    arguments.frequency_hz = 473142857U;
    arguments.tune_timeout_ms = 3000U;
    arguments.output = "-";
    arguments.packet_count_set = true;
    arguments.packet_count = 32U;

    px4::userland::cli::Px4TsFailureKind failure =
        px4::userland::cli::Px4TsFailureKind::ipc;
    px4::userland::cli::Px4TsRunDiagnostics diagnostics{};
    {
        px4::userland::cli::Px4TsFdOutput output(fd, true);
        V1_CHECK(output.ready());
        RecordingClientClock clock;
        RecordingClientSignal signal;
        const auto result = px4::userland::cli::Px4TsRunner::run_wire(
            arguments, output, clock, signal, &failure, &diagnostics);
        if (expect_success) {
            if (!result) std::fprintf(stderr, "product recording runner: %s\n",
                                      error_string(result.error()));
            V1_CHECK(result);
        } else {
            V1_CHECK(!result && result.error() == expected_error);
        }
    }
    if (expect_success) {
        V1_CHECK(read_recording_file(path) == recording_packets(32U));
        V1_CHECK(diagnostics.stream_end_valid && diagnostics.packets_written == 32U);
        V1_CHECK(total(fixture.backend.open_calls) != 0U);
    } else {
        V1_CHECK(total(fixture.backend.open_calls) == 0U);
        V1_CHECK(total(fixture.backend.hz_calls) == 0U);
        V1_CHECK(read_recording_file(path).empty());
    }
    V1_CHECK(fixture.server_ok.load());
    return true;
}

bool test_product_device_only_usb_recording()
{
    return product_recording_case("product-device-only", "usb:3-1.1", false, true, Error::OK);
}

bool test_product_device_instance_recording()
{
    return product_recording_case("product-device-instance", "usb:3-1.1", true, true, Error::OK);
}

bool test_product_instance_only_recording()
{
    return product_recording_case("product-instance-only", nullptr, true, true, Error::OK);
}

bool test_product_device_instance_mismatch_no_acquire()
{
    return product_recording_case("product-device-mismatch", "usb:9-9.9", true, false,
                                  Error::NOT_FOUND);
}

bool test_product_serial_absence_not_found()
{
    return product_recording_case("product-serial-absent", "serial:00001205000960", false,
                                  false, Error::NOT_FOUND);
}

bool card_untouched(const Fixture& f)
{
    return f.card_backend.power_calls.load() == 0U && f.card_backend.uart_calls.load() == 0U &&
           f.card_backend.detect_calls.load() == 0U &&
           f.card_session.initialize_calls.load() == 0U &&
           f.card_session.transmit_calls.load() == 0U;
}

bool test_mock_factory_context_reaches_server()
{
    TempRuntime runtime;
    V1_CHECK(runtime.valid());
    const auto* profile = asicen::find_profile(asicen::ModelId::W3u3);
    V1_CHECK(profile != nullptr);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    asicen::MockTunerBackend backend(2U);
    Nonce nonce;
    LeaseIds leases;
    Clock clock;
    TunerService tuner(backend, nonce, clock, &leases);
    const EndpointConfig endpoint{runtime.path().c_str(), "mock-factory",
                                  kControlEndpointName, EndpointAccess::private_user};
    ControlContext context;
    V1_CHECK(asicen::build_asicen_mock_control_context(*profile, tuner,
        "mock-factory", make_epoch(0x4aU), context));
    auto created = PosixControlServer::create(endpoint, card, tuner, context);
    V1_CHECK(created);
    auto server = std::move(created.value());
    std::atomic<bool> stop{false};
    std::thread poller([&]() {
        while (!stop.load()) (void)server->poll_once(Timeout{10U});
    });
    auto client = TestClient::connect(endpoint);
    wire::ListResponse list;
    const bool served = client && client->hello() && client->list(list) &&
                        list.receivers.value.count == 2U && list.functions.count == 2U;
    stop.store(true);
    poller.join();
    const auto server_stopped = server->shutdown();
    const auto card_stopped = card.shutdown();
    return served && server_stopped && card_stopped &&
        card_backend.power_calls.load() == 0U &&
        card_backend.uart_calls.load() == 0U && card_backend.detect_calls.load() == 0U &&
        card_session.initialize_calls.load() == 0U &&
        card_session.transmit_calls.load() == 0U;
}

bool contains(const std::vector<std::uint8_t>& haystack, const std::uint8_t* needle,
              std::size_t size)
{
    if (size == 0U || haystack.size() < size) return false;
    for (std::size_t i = 0U; i + size <= haystack.size(); ++i) {
        if (std::memcmp(haystack.data() + i, needle, size) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 1. Framing and connection rules

bool test_framing_and_handshake()
{
    Fixture f("v1-framing");
    V1_CHECK(f.start());

    // HELLO split byte by byte across header and payload.
    {
        auto client = TestClient::connect(f.endpoint);
        V1_CHECK(client);
        wire::HelloRequest hello;
        hello.min = wire::Version{1U, 0U};
        hello.max = wire::Version{1U, 0U};
        const auto payload = TestClient::encode(hello, &wire::encode_hello_request);
        const auto bytes = TestClient::frame(wire::kOpcodeHello, 1U,
                                             ByteView{payload.data(), payload.size()});
        for (std::size_t i = 0U; i < bytes.size(); ++i) {
            V1_CHECK(client->send_bytes(bytes.data() + i, 1U));
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        Reply reply;
        V1_CHECK(client->read_frame(reply) && reply.header.kind == wire::Kind::response &&
                 reply.header.opcode == wire::kOpcodeHello && reply.header.request_id == 1U);
        const auto decoded = wire::decode_hello_response(reply.view());
        V1_CHECK(decoded && decoded.value().epoch == make_epoch(0x10U) &&
                 decoded.value().features == 0U && decoded.value().major == 1U);
    }
    // Header in one write, payload coalesced with nothing else in a second.
    {
        auto client = TestClient::connect(f.endpoint);
        V1_CHECK(client && client->hello());
        wire::ListRequest list;
        list.epoch = client->epoch;
        const auto payload = TestClient::encode(list, &wire::encode_list_request);
        const auto bytes = TestClient::frame(wire::kOpcodeList, 2U,
                                             ByteView{payload.data(), payload.size()});
        V1_CHECK(client->send_bytes(bytes.data(), 7U));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        V1_CHECK(client->send_bytes(bytes.data() + 7U, bytes.size() - 7U));
        Reply reply;
        V1_CHECK(client->read_frame(reply) && reply.header.kind == wire::Kind::response &&
                 reply.header.opcode == wire::kOpcodeList);
    }
    // A request pipelined behind an unanswered one breaks the single
    // outstanding request rule: the connection closes after the first reply.
    {
        auto client = TestClient::connect(f.endpoint);
        V1_CHECK(client && client->hello());
        wire::ListRequest list;
        list.epoch = client->epoch;
        const auto payload = TestClient::encode(list, &wire::encode_list_request);
        auto bytes = TestClient::frame(wire::kOpcodeList, 2U,
                                       ByteView{payload.data(), payload.size()});
        const auto second = TestClient::frame(wire::kOpcodeList, 3U,
                                              ByteView{payload.data(), payload.size()});
        bytes.insert(bytes.end(), second.begin(), second.end());
        V1_CHECK(client->send_bytes(bytes.data(), bytes.size()));
        V1_CHECK(client->closed_without_reply());
    }
    // HELLO is required first.
    {
        auto client = TestClient::connect(f.endpoint);
        V1_CHECK(client);
        wire::ListRequest list;
        list.epoch = make_epoch(0x10U);
        V1_CHECK(client->send_request(wire::kOpcodeList,
                                      TestClient::encode(list, &wire::encode_list_request)));
        V1_CHECK(client->closed_without_reply());
    }
    // Legacy PX4U magic is never interpreted: closed without a reply.
    {
        auto client = TestClient::connect(f.endpoint);
        V1_CHECK(client);
        auto bytes = TestClient::frame(wire::kOpcodeHello, 1U, ByteView{nullptr, 0U});
        bytes[0] = 'P';
        bytes[1] = 'X';
        bytes[2] = '4';
        bytes[3] = 'U';
        V1_CHECK(client->send_bytes(bytes.data(), bytes.size()));
        V1_CHECK(client->closed_without_reply());
    }
    // Request ID 0 is a malformed envelope; an equal ID is a protocol error.
    {
        auto client = TestClient::connect(f.endpoint);
        V1_CHECK(client && client->hello());
        auto bytes = TestClient::frame(wire::kOpcodeList, 9U, ByteView{nullptr, 0U});
        bytes[12] = bytes[13] = bytes[14] = bytes[15] = 0U;
        V1_CHECK(client->send_bytes(bytes.data(), bytes.size()));
        V1_CHECK(client->closed_without_reply());

        auto repeat = TestClient::connect(f.endpoint);
        V1_CHECK(repeat && repeat->hello());
        wire::ListRequest list;
        list.epoch = repeat->epoch;
        const auto payload = TestClient::encode(list, &wire::encode_list_request);
        const auto same = TestClient::frame(wire::kOpcodeList, repeat->last_request_id(),
                                            ByteView{payload.data(), payload.size()});
        V1_CHECK(repeat->send_bytes(same.data(), same.size()));
        V1_CHECK(repeat->closed_without_reply());
    }
    // Unknown opcode with a valid envelope is UNSUPPORTED and the connection
    // continues; a malformed payload of a served opcode closes it.
    {
        auto client = TestClient::connect(f.endpoint);
        V1_CHECK(client && client->hello());
        Reply reply;
        V1_CHECK(client->request(0x0123U, std::vector<std::uint8_t>{1U, 2U}, reply));
        V1_CHECK(TestClient::error_code(reply) ==
                 static_cast<std::uint32_t>(Error::UNSUPPORTED));
        wire::ListResponse list;
        V1_CHECK(client->list(list));
        V1_CHECK(client->send_request(wire::kOpcodeList, std::vector<std::uint8_t>{1U, 2U, 3U}));
        V1_CHECK(client->closed_without_reply());
    }
    // Version and required-feature failures answer HELLO and close.
    {
        auto client = TestClient::connect(f.endpoint);
        V1_CHECK(client);
        wire::HelloRequest hello;
        hello.min = wire::Version{2U, 0U};
        hello.max = wire::Version{2U, 0U};
        Reply reply;
        V1_CHECK(client->request(wire::kOpcodeHello,
                                 TestClient::encode(hello, &wire::encode_hello_request), reply));
        V1_CHECK(TestClient::error_code(reply) ==
                 static_cast<std::uint32_t>(Error::VERSION_MISMATCH));
        V1_CHECK(client->closed_without_reply());

        auto card = TestClient::connect(f.endpoint);
        V1_CHECK(card);
        // CARD_SERVICE is served now; STATE_EVENT is still unconnected and a
        // required bit outside the offered set must be refused.
        V1_CHECK(!card->hello(nullptr, wire::kFeatureStateEvent));
        V1_CHECK(card->closed_without_reply());
    }
    // Clean EOF and EOF inside a frame both end the connection; the truncated
    // request is never dispatched.
    {
        const std::size_t opens_before = total(f.backend.open_calls);
        auto clean = TestClient::connect(f.endpoint);
        auto truncated = TestClient::connect(f.endpoint);
        V1_CHECK(clean && truncated && clean->hello() && truncated->hello());
        V1_CHECK(wait_for(kWaitBound, [&]() { return f.server->connection_count() >= 2U; }));
        const std::size_t connected = f.server->connection_count();
        wire::AcquireRequest acquire;
        acquire.epoch = truncated->epoch;
        acquire.receiver_id = 2U;
        const auto payload = TestClient::encode(acquire, &wire::encode_acquire_request);
        const auto bytes = TestClient::frame(wire::kOpcodeAcquire, 9U,
                                             ByteView{payload.data(), payload.size()});
        V1_CHECK(truncated->send_bytes(bytes.data(), bytes.size() - 3U));
        clean->close();
        truncated->close();
        V1_CHECK(wait_for(kWaitBound,
                          [&]() { return f.server->connection_count() + 2U <= connected; }));
        V1_CHECK(total(f.backend.open_calls) == opens_before);
    }
    V1_CHECK(f.server_ok.load());
    V1_CHECK(card_untouched(f));
    return true;
}

// Partial response write: the request stays outstanding until its last byte
// is written, so a request sent while the reply is incomplete is refused.
struct PartialObservation final {
    std::atomic<std::size_t> partial{0U};
    Gate gate;
    static void on_partial(void* context, std::uint64_t) noexcept
    {
        auto& observation = *static_cast<PartialObservation*>(context);
        observation.partial.fetch_add(1U);
        observation.gate.pass();
    }
};

[[maybe_unused]] bool test_partial_response_write()
{
    Fixture f("v1-partial");
    V1_CHECK(f.start());
    PartialObservation observation;
    observation.gate.arm();
    ControlServerTestHooks hooks{};
    hooks.control_write_limit = 3U;
    hooks.control_write_partial = &PartialObservation::on_partial;
    hooks.context = &observation;
    set_control_server_test_hooks(&hooks);
    struct Reset final {
        Gate& gate;
        ~Reset()
        {
            gate.open();
            set_control_server_test_hooks(nullptr);
        }
    } reset{observation.gate};

    auto client = TestClient::connect(f.endpoint);
    V1_CHECK(client);
    wire::HelloRequest hello;
    hello.min = wire::Version{1U, 0U};
    hello.max = wire::Version{1U, 0U};
    V1_CHECK(client->send_request(wire::kOpcodeHello,
                                  TestClient::encode(hello, &wire::encode_hello_request)));
    V1_CHECK(observation.gate.wait_entered());
    wire::ListRequest list;
    list.epoch = make_epoch(0x10U);
    V1_CHECK(client->send_request(wire::kOpcodeList,
                                  TestClient::encode(list, &wire::encode_list_request)));
    observation.gate.open();
    // The partial HELLO reply may still finish, but the second request is
    // never answered and the connection closes.
    Reply reply;
    bool listed = false;
    while (client->read_frame(reply, std::chrono::milliseconds(3000))) {
        if (reply.header.opcode == wire::kOpcodeList) listed = true;
    }
    V1_CHECK(!listed);
    V1_CHECK(client->closed_without_reply(std::chrono::milliseconds(100)));

    // The same limit with a client that waits for each full reply works.
    auto patient = TestClient::connect(f.endpoint);
    V1_CHECK(patient && patient->hello());
    wire::ListResponse listed_response;
    V1_CHECK(patient->list(listed_response));
    V1_CHECK(f.server_ok.load());
    return true;
}

// ---------------------------------------------------------------------------
// 2. HELLO -> LIST -> STATUS on one connection

bool test_list_status_one_context()
{
    Fixture f("v1-status");
    V1_CHECK(f.start());
    auto client = TestClient::connect(f.endpoint);
    V1_CHECK(client);
    wire::HelloResponse hello;
    V1_CHECK(client->hello(&hello));
    std::unique_ptr<wire::ListResponse> list(new wire::ListResponse());
    V1_CHECK(client->list(*list));
    V1_CHECK(list->epoch == hello.epoch);
    V1_CHECK(wire::validate_list_response(*list) == dtv::userland::Error::OK);
    // ASICEN paired topology exposes its two operational primary receivers;
    // the second claimed USB function remains listed and reserved.
    V1_CHECK(list->functions.count == 2U && list->receivers.present &&
             list->receivers.value.count == 2U && list->readers.present &&
             list->readers.value.count == 1U && list->readers.value.items[0].id == 0U);
    V1_CHECK(list->topology.state == wire::TopologyState::complete);
    V1_CHECK(list->topology.expected_function_count.present &&
             list->topology.expected_function_count.value == 2U);
    V1_CHECK(list->enclosure.present &&
             list->enclosure.value.identity.serial_state == wire::IdentitySerialState::absent &&
             list->enclosure.value.identity.uniqueness == wire::Uniqueness::not_applicable);
    V1_CHECK(list->functions.items[1].location.usb_path.present &&
             std::string(list->functions.items[1].location.usb_path.value.bytes.data(),
                         list->functions.items[1].location.usb_path.value.length) == "3-1.2");
    for (std::uint16_t id = 0U; id < 2U; ++id) {
        const wire::ReceiverDescriptor& receiver = list->receivers.value.items[id];
        const bool satellite = id == 0U;
        V1_CHECK(receiver.id == id && receiver.binding.present &&
                 receiver.binding.value.function_id == 0U &&
                 receiver.binding.value.local_id == id &&
                 receiver.systems.mask == (satellite ? 0x02U : 0x01U) &&
                 receiver.capabilities.measurement_mask == 0U &&
                 ((receiver.capabilities.controls & wire::kControlBitLnb) != 0U) == satellite);
    }
    V1_CHECK(list->producer.version.length != 0U);

    std::unique_ptr<wire::StatusResponse> status(new wire::StatusResponse());
    Reply reply;
    V1_CHECK(client->status(wire::Scope{wire::ScopeKind::enclosure, 0U}, reply, status.get()));
    V1_CHECK(reply.header.kind == wire::Kind::response);
    V1_CHECK(status->context.epoch == hello.epoch && status->context.instance.present &&
             status->context.state == wire::ContextState::ready);
    V1_CHECK(status->snapshot.sequence > list->snapshot.sequence);
    wire::StatusContext status_context;
    status_context.list = list.get();
    V1_CHECK(wire::validate_status_response(*status, status_context) == dtv::userland::Error::OK);
    V1_CHECK(status->receivers.present && status->receivers.value.count == 2U);
    for (std::size_t i = 0U; i < 2U; ++i) {
        const wire::ReceiverStatus& receiver = status->receivers.value.items[i];
        V1_CHECK(receiver.present && receiver.state == wire::ReceiverState::free &&
                 !receiver.leased && receiver.operation == wire::Operation::idle &&
                 !receiver.current_tune.present && receiver.tune_revision == 0U);
    }
    // P's real reader is listed, with no cached sample and no handle data.
    V1_CHECK(status->readers.present && status->readers.value.count == 1U);
    const wire::ReaderStatus& reader = status->readers.value.items[0];
    V1_CHECK(reader.present && reader.card.state == wire::CardState::unknown &&
             !reader.card.sampled_at_ns.present &&
             reader.card.reason == static_cast<std::uint8_t>(wire::UnknownReason::not_sampled) &&
             reader.handles_open == 0U);
    // JSON from the exact same typed snapshot validates against the LIST.
    std::vector<std::uint8_t> json(70000U);
    const auto json_size = wire::encode_status_json(
        *status, status_context, MutableByteView{json.data(), json.size()});
    V1_CHECK(json_size && json_size.value() > 0U);
    const std::string json_text(json.begin(), json.begin() + json_size.value());
    V1_CHECK(json_text.find("\"readers\"") != std::string::npos &&
             json_text.find("not_sampled") != std::string::npos);
    V1_CHECK(total(f.backend.open_calls) == 0U && total(f.backend.hz_calls) == 0U &&
             total(f.backend.power_calls) == 0U && card_untouched(f));

    // A lease never appears in STATUS: only leased=true.
    const std::uint64_t lease = client->acquire_lease(1U);
    V1_CHECK(lease != 0U);
    V1_CHECK(client->status(wire::Scope{wire::ScopeKind::receiver, 1U}, reply, status.get()));
    V1_CHECK(reply.header.kind == wire::Kind::response && status->receivers.value.count == 1U &&
             status->receivers.value.items[0].leased &&
             status->receivers.value.items[0].state == wire::ReceiverState::leased);
    std::array<std::uint8_t, 8U> lease_bytes{};
    for (std::size_t i = 0U; i < 8U; ++i)
        lease_bytes[i] = static_cast<std::uint8_t>(lease >> (8U * i));
    V1_CHECK(!contains(reply.payload, lease_bytes.data(), lease_bytes.size()));
    const std::array<std::uint8_t, ipc::kNonceLength> token{
        0x01U, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU,
        0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU, 0x5aU};
    V1_CHECK(!contains(reply.payload, token.data(), token.size()));

    // Wrong epoch, unknown public ID and invalid scope fail before any
    // backend or card callback.
    const std::size_t opens = total(f.backend.open_calls);
    const wire::Uuid stale = make_epoch(0x77U);
    V1_CHECK(client->status(wire::Scope{wire::ScopeKind::enclosure, 0U}, reply, nullptr, &stale));
    V1_CHECK(TestClient::error_code(reply) == wire::kWireErrorStaleHandle);
    V1_CHECK(client->status(wire::Scope{wire::ScopeKind::receiver, 8U}, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::NOT_FOUND));
    V1_CHECK(client->status(wire::Scope{wire::ScopeKind::reader, 1U}, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::NOT_FOUND));
    V1_CHECK(client->status(wire::Scope{wire::ScopeKind::stream, 1U}, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::NOT_FOUND));
    V1_CHECK(client->status(wire::Scope{wire::ScopeKind::reader, 0U}, reply, status.get()));
    V1_CHECK(reply.header.kind == wire::Kind::response && status->readers.value.count == 1U &&
             !status->receivers.present && !status->functions.present);
    Reply acquire_reply;
    V1_CHECK(client->acquire(3U, acquire_reply, &stale));
    V1_CHECK(TestClient::error_code(acquire_reply) == wire::kWireErrorStaleHandle);
    V1_CHECK(total(f.backend.open_calls) == opens);
    V1_CHECK(card_untouched(f));
    V1_CHECK(f.server_ok.load());
    return true;
}

[[maybe_unused]] bool test_sparse_ids_and_empty_readers()
{
    Fixture f("v1-sparse");
    V1_CHECK(f.start(true, 0x20U));
    auto client = TestClient::connect(f.endpoint);
    V1_CHECK(client && client->hello());
    std::unique_ptr<wire::ListResponse> list(new wire::ListResponse());
    V1_CHECK(client->list(*list));
    V1_CHECK(list->receivers.value.count == 3U && list->receivers.value.items[0].id == 0U &&
             list->receivers.value.items[2].id == 513U && list->readers.present &&
             list->readers.value.count == 0U);
    std::unique_ptr<wire::StatusResponse> status(new wire::StatusResponse());
    Reply reply;
    V1_CHECK(client->status(wire::Scope{wire::ScopeKind::enclosure, 0U}, reply, status.get()));
    V1_CHECK(reply.header.kind == wire::Kind::response && status->readers.present &&
             status->readers.value.count == 0U && status->receivers.value.count == 3U);
    V1_CHECK(client->status(wire::Scope{wire::ScopeKind::reader, 0U}, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::NOT_FOUND));
    // Public ID 1 is not published; 513 maps to service receiver 6, 0 to 2.
    V1_CHECK(client->acquire(1U, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::NOT_FOUND));
    V1_CHECK(total(f.backend.open_calls) == 0U);
    const std::uint64_t high = client->acquire_lease(513U);
    const std::uint64_t zero = client->acquire_lease(0U);
    V1_CHECK(high != 0U && zero != 0U);
    V1_CHECK(f.backend.open_calls[6].load() == 1U && f.backend.open_calls[2].load() == 1U &&
             total(f.backend.open_calls) == 2U);
    V1_CHECK(client->status(wire::Scope{wire::ScopeKind::receiver, 513U}, reply, status.get()));
    V1_CHECK(reply.header.kind == wire::Kind::response &&
             status->receivers.value.items[0].descriptor.id == 513U &&
             status->receivers.value.items[0].leased);
    Reply tuned;
    V1_CHECK(client->tune(client->terrestrial(high, 557142857U), tuned));
    V1_CHECK(tuned.header.kind == wire::Kind::response);
    V1_CHECK(f.backend.last_hz[6].load() == 557142857U && f.backend.hz_calls[2].load() == 0U);
    V1_CHECK(f.server_ok.load());
    return true;
}

// ---------------------------------------------------------------------------
// 3. ACQUIRE -> TUNE -> RELEASE through the real workers and engine

bool tune_result(const Reply& reply, wire::TuneResult& out)
{
    if (reply.header.kind != wire::Kind::response) return false;
    auto decoded = wire::decode_tune_result(reply.view());
    if (!decoded) return false;
    out = decoded.value();
    return true;
}

bool release_result(const Reply& reply, wire::ReleaseResponse& out)
{
    if (reply.header.kind != wire::Kind::response) return false;
    auto decoded = wire::decode_release_response(reply.view());
    if (!decoded) return false;
    out = decoded.value();
    return true;
}

bool test_tune_exact_hz_and_release()
{
    Fixture f("v1-tune");
    V1_CHECK(f.start());
    auto owner = TestClient::connect(f.endpoint);
    auto other = TestClient::connect(f.endpoint);
    V1_CHECK(owner && other && owner->hello() && other->hello());
    const std::uint64_t lease = owner->acquire_lease(1U);
    V1_CHECK(lease != 0U);
    Reply reply;
    V1_CHECK(other->acquire(1U, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::BUSY));

    // Fractional-kHz exact Hz request reaches the strict entrance unchanged.
    V1_CHECK(owner->tune(owner->terrestrial(lease, 473142857U), reply));
    wire::TuneResult result;
    V1_CHECK(tune_result(reply, result));
    V1_CHECK(result.epoch == owner->epoch && result.tune_revision == 1U &&
             result.requested_hz == 473142857U && result.programmed_hz.present &&
             result.programmed_hz.value == 473142000U &&
             result.verification == wire::Verification::lock &&
             result.lock.state == wire::MeasureState::valid && result.lock.value &&
             !result.selected_tsid.present && !result.selected_slot.present);
    V1_CHECK(f.backend.last_hz[1].load() == 473142857U && f.backend.khz_calls.load() == 0U);

    // Profile boundaries: 40 MHz is admitted, one Hz below is UNSUPPORTED
    // before any power or frontend call and spends no revision.
    V1_CHECK(owner->tune(owner->terrestrial(lease, 40000000U), reply));
    V1_CHECK(tune_result(reply, result) && result.tune_revision == 2U &&
             result.requested_hz == 40000000U);
    const std::size_t power_before = f.backend.power_calls[1].load();
    V1_CHECK(owner->tune(owner->terrestrial(lease, 39999999U), reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::UNSUPPORTED));
    V1_CHECK(f.backend.power_calls[1].load() == power_before);
    // LNA, PID subset and satellite automatic selection are not provided.
    wire::TuneRequest lna = owner->terrestrial(lease, 473142857U);
    lna.lna.present = true;
    lna.lna.value = true;
    V1_CHECK(owner->tune(lna, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::UNSUPPORTED));

    std::unique_ptr<wire::StatusResponse> status(new wire::StatusResponse());
    V1_CHECK(owner->status(wire::Scope{wire::ScopeKind::receiver, 1U}, reply, status.get()));
    const wire::ReceiverStatus& tuned = status->receivers.value.items[0];
    V1_CHECK(tuned.state == wire::ReceiverState::tuned && tuned.leased &&
             tuned.tune_revision == 2U && tuned.current_tune.present &&
             tuned.current_tune.value.revision == 2U &&
             tuned.current_tune.value.frequency_hz == 40000000U &&
             tuned.last_successful_tune.present);

    // Another owner's lease and a stale epoch never reach hardware.
    const std::size_t hz_before = total(f.backend.hz_calls);
    V1_CHECK(other->tune(other->terrestrial(lease, 473142857U), reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::NOT_FOUND));
    wire::TuneRequest stale = owner->terrestrial(lease, 473142857U);
    stale.epoch = make_epoch(0x66U);
    V1_CHECK(owner->tune(stale, reply));
    V1_CHECK(TestClient::error_code(reply) == wire::kWireErrorStaleHandle);
    V1_CHECK(other->release(lease, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::NOT_FOUND));
    V1_CHECK(total(f.backend.hz_calls) == hz_before && f.backend.close_calls[1].load() == 0U);

    // Satellite slot selection on receiver 0.
    const std::uint64_t satellite = owner->acquire_lease(0U);
    V1_CHECK(satellite != 0U);
    wire::TuneRequest bs;
    bs.epoch = owner->epoch;
    bs.lease_id = satellite;
    bs.system = wire::System::isdb_s;
    bs.frequency_hz = 1318000000U;
    bs.selection = wire::Selection::slot;
    bs.selection_value = 3U;
    bs.lnb_voltage.present = true;
    bs.lnb_voltage.value = 0U;
    bs.timeout_ms = 1000U;
    const std::size_t satellite_power_before = f.backend.power_calls[0].load();
    wire::TuneRequest bad_slot = bs;
    bad_slot.selection_value = 8U;
    V1_CHECK(owner->tune(bad_slot, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::UNSUPPORTED));
    wire::TuneRequest bad_lnb = bs;
    bad_lnb.lnb_voltage.value = 15U;
    V1_CHECK(owner->tune(bad_lnb, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::UNSUPPORTED));
    V1_CHECK(f.backend.power_calls[0].load() == satellite_power_before);
    V1_CHECK(owner->tune(bs, reply));
    V1_CHECK(tune_result(reply, result) && result.selected_slot.present &&
             result.selected_slot.value == 3U && result.requested_hz == 1318000000U);
    V1_CHECK(f.backend.last_voltage.load() == 0U);

    // RELEASE reports the actual released/quiesced outcome.
    wire::ReleaseResponse released;
    V1_CHECK(owner->release(lease, reply));
    V1_CHECK(release_result(reply, released) && released.released && released.quiesced &&
             !released.error.present && released.lease_id == lease &&
             released.epoch == owner->epoch);
    V1_CHECK(owner->release(lease, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::NOT_FOUND));
    V1_CHECK(other->acquire_lease(1U) != 0U);

    // A failing close is neither released nor quiesced and keeps the
    // receiver excluded.
    f.backend.close_error.store(Error::USB_IO);
    V1_CHECK(owner->release(satellite, reply));
    V1_CHECK(release_result(reply, released) && !released.released && !released.quiesced &&
             released.error.present &&
             released.error.value.code == static_cast<std::uint32_t>(Error::USB_IO));
    f.backend.close_error.store(Error::OK);
    V1_CHECK(owner->status(wire::Scope{wire::ScopeKind::receiver, 0U}, reply, status.get()));
    V1_CHECK(status->receivers.value.items[0].state == wire::ReceiverState::error &&
             !status->receivers.value.items[0].leased);
    V1_CHECK(other->acquire(0U, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::NOT_READY));
    V1_CHECK(f.backend.capture_calls.load() == 0U);
    V1_CHECK(card_untouched(f));
    V1_CHECK(f.server_ok.load());
    return true;
}

bool test_unnegotiated_pid_filter_stops_before_backend()
{
    Fixture f("v1-pid-feature");
    V1_CHECK(f.start());
    auto client = TestClient::connect(f.endpoint);
    V1_CHECK(client && client->hello());
    const std::uint64_t lease = client->acquire_lease(1U);
    V1_CHECK(lease != 0U);
    wire::TuneRequest tune = client->terrestrial(lease, 473142857U);
    tune.pids.present = true;
    tune.pids.value.count = 1U;
    tune.pids.value.items[0] = 256U;
    const std::size_t hz_before = total(f.backend.hz_calls);
    const std::size_t power_before = total(f.backend.power_calls);
    Reply reply;
    V1_CHECK(client->tune(tune, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::UNSUPPORTED));
    V1_CHECK(total(f.backend.hz_calls) == hz_before &&
             total(f.backend.power_calls) == power_before);
    V1_CHECK(f.server_ok.load());
    return true;
}

// The acceptance sample precedes queue publication: a ticket that waits in
// its lane past its budget is TIMEOUT without any power or frontend I/O.
[[maybe_unused]] bool test_queue_wait_counts_against_deadline()
{
    Fixture f("v1-queue");
    V1_CHECK(f.start());
    auto blocker = TestClient::connect(f.endpoint);
    auto waiter = TestClient::connect(f.endpoint);
    V1_CHECK(blocker && waiter && blocker->hello() && waiter->hello());
    // Receivers 2 and 3 share the first bridge lane.
    const std::uint64_t held = blocker->acquire_lease(3U);
    const std::uint64_t queued = waiter->acquire_lease(2U);
    V1_CHECK(held != 0U && queued != 0U);
    f.backend.gated_tune_receiver.store(3U);
    f.backend.tune_gate.arm();
    V1_CHECK(blocker->send_request(
        wire::kOpcodeTune,
        TestClient::encode(blocker->terrestrial(held, 473142857U, 5000U),
                           &wire::encode_tune_request)));
    V1_CHECK(f.backend.tune_gate.wait_entered());
    V1_CHECK(waiter->send_request(
        wire::kOpcodeTune,
        TestClient::encode(waiter->terrestrial(queued, 473142857U, 100U),
                           &wire::encode_tune_request)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    f.clock.now.fetch_add(500U);
    f.backend.tune_gate.open();
    Reply reply;
    V1_CHECK(waiter->read_frame(reply) && reply.header.opcode == wire::kOpcodeTune);
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::TIMEOUT));
    V1_CHECK(f.backend.hz_calls[2].load() == 0U && f.backend.power_calls[2].load() == 0U);
    V1_CHECK(blocker->read_frame(reply) && reply.header.opcode == wire::kOpcodeTune);
    V1_CHECK(f.server_ok.load());
    return true;
}

bool test_asicen_same_lane_exclusion()
{
    Fixture f("asicen-lanes");
    V1_CHECK(f.start());
    auto first = TestClient::connect(f.endpoint);
    auto second = TestClient::connect(f.endpoint);
    V1_CHECK(first && second && first->hello() && second->hello());
    f.backend.gated_open_receiver.store(0U);
    f.backend.open_gate.arm();
    wire::AcquireRequest acquire_zero;
    acquire_zero.epoch = first->epoch;
    acquire_zero.receiver_id = 0U;
    wire::AcquireRequest acquire_one;
    acquire_one.epoch = second->epoch;
    acquire_one.receiver_id = 1U;
    V1_CHECK(first->send_request(wire::kOpcodeAcquire,
        TestClient::encode(acquire_zero, &wire::encode_acquire_request)));
    V1_CHECK(f.backend.open_gate.wait_entered());
    V1_CHECK(second->send_request(wire::kOpcodeAcquire,
        TestClient::encode(acquire_one, &wire::encode_acquire_request)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    V1_CHECK(!f.backend.open_entered[1].load());
    f.backend.open_gate.open();
    Reply reply;
    V1_CHECK(first->read_frame(reply) && reply.header.kind == wire::Kind::response);
    V1_CHECK(second->read_frame(reply) && reply.header.kind == wire::Kind::response);
    V1_CHECK(f.backend.open_calls[0].load() == 1U && f.backend.open_calls[1].load() == 1U);
    V1_CHECK(card_untouched(f));
    return true;
}

bool test_request_stop_precedes_worker_drain()
{
    Fixture f("asicen-stop-order");
    V1_CHECK(f.start());
    auto client = TestClient::connect(f.endpoint);
    V1_CHECK(client && client->hello());
    f.backend.gated_open_receiver.store(0U);
    f.backend.open_gate.arm();
    V1_CHECK(client->send_request(wire::kOpcodeAcquire,
        TestClient::encode(wire::AcquireRequest{client->epoch, 0U},
                           &wire::encode_acquire_request)));
    V1_CHECK(f.backend.open_gate.wait_entered());
    f.stop.store(true);
    if (f.poller.joinable()) f.poller.join();
    std::atomic<bool> shutdown_done{false};
    Result<void> shutdown_result = Result<void>::failure(Error::INTERNAL);
    std::thread shutdown([&]() {
        shutdown_result = f.server->shutdown();
        shutdown_done.store(true);
    });
    V1_CHECK(wait_for(kWaitBound, [&]() { return f.stop_hooks.calls.load() == 4U; }));
    V1_CHECK(wait_for(kWaitBound, [&]() { return shutdown_done.load(); }));
    shutdown.join();
    V1_CHECK(shutdown_result && f.backend.open_returned_after_stop.load() &&
             f.stop_hooks.calls.load() == 4U &&
             f.card_backend.stop_calls.load() == 1U &&
             f.card_session.stop_calls.load() == 1U &&
             f.stream_control.stop_calls.load() == 1U);
    return true;
}

// A publication signal failure aborts the admitted ticket without I/O.
[[maybe_unused]] bool test_publication_signal_failure()
{
    ControlWorkerStartupOptions options{};
    options.fail_tune_signal = true;
    ControlWorkerLanes::set_test_startup_options(&options);
    Fixture f("v1-signal");
    const bool started = f.start();
    ControlWorkerLanes::set_test_startup_options(nullptr);
    V1_CHECK(started);
    auto client = TestClient::connect(f.endpoint);
    V1_CHECK(client && client->hello());
    const std::uint64_t lease = client->acquire_lease(2U);
    V1_CHECK(lease != 0U);
    Reply reply;
    V1_CHECK(client->tune(client->terrestrial(lease, 473142857U), reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::INTERNAL));
    V1_CHECK(f.backend.hz_calls[2].load() == 0U && f.backend.power_calls[2].load() == 0U);
    std::unique_ptr<wire::StatusResponse> status(new wire::StatusResponse());
    V1_CHECK(client->status(wire::Scope{wire::ScopeKind::receiver, 2U}, reply, status.get()));
    const wire::ReceiverStatus& receiver = status->receivers.value.items[0];
    V1_CHECK(!receiver.current_tune.present && receiver.tune_revision == 1U &&
             receiver.operation == wire::Operation::idle && receiver.leased);
    V1_CHECK(f.server_ok.load());
    return true;
}

// Owner EOF while its tune is in the frontend: the current tune is
// invalidated before the hardware cleanup, no late success is published,
// and the receiver is not reacquirable until the cleanup completes.
bool test_owner_eof_during_tune_and_blocked_cleanup()
{
    Fixture f("v1-eof");
    V1_CHECK(f.start());
    auto owner = TestClient::connect(f.endpoint);
    auto monitor = TestClient::connect(f.endpoint);
    V1_CHECK(owner && monitor && owner->hello() && monitor->hello());
    const std::uint64_t lease = owner->acquire_lease(1U);
    V1_CHECK(lease != 0U);
    Reply reply;
    V1_CHECK(owner->tune(owner->terrestrial(lease, 473142857U), reply));
    V1_CHECK(reply.header.kind == wire::Kind::response);

    f.backend.gated_tune_receiver.store(1U);
    f.backend.tune_gate.arm();
    f.backend.gated_close_receiver.store(1U);
    f.backend.close_gate.arm();
    V1_CHECK(owner->send_request(
        wire::kOpcodeTune,
        TestClient::encode(owner->terrestrial(lease, 557142857U, 5000U),
                           &wire::encode_tune_request)));
    V1_CHECK(f.backend.tune_gate.wait_entered());
    V1_CHECK(::shutdown(owner->fd(), SHUT_RDWR) == 0);
    std::unique_ptr<wire::StatusResponse> status(new wire::StatusResponse());
    // Wait until the poll thread has observed EOF and synchronously revoked
    // the owner's ticket; starting the in-flight tune already invalidates the
    // old current record and is not proof of disconnect cleanup.
    V1_CHECK(wait_for(kWaitBound, [&]() { return f.server->connection_count() == 1U; }));
    const auto revoked = f.tuner->tune_metadata(1U);
    V1_CHECK(revoked && !revoked.value().current_valid);
    V1_CHECK(f.backend.close_calls[1].load() == 0U);
    f.backend.tune_gate.open();
    V1_CHECK(f.backend.close_gate.wait_entered());
    V1_CHECK(monitor->status(wire::Scope{wire::ScopeKind::receiver, 1U}, reply, status.get()));
    const wire::ReceiverStatus& cleaning = status->receivers.value.items[0];
    V1_CHECK(cleaning.leased && cleaning.operation == wire::Operation::releasing &&
             !cleaning.current_tune.present && cleaning.last_successful_tune.present &&
             cleaning.last_successful_tune.value.frequency_hz == 473142857U);
    V1_CHECK(monitor->acquire(1U, reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::BUSY));
    f.backend.close_gate.open();
    V1_CHECK(wait_for(kWaitBound, [&]() { return f.backend.close_calls[1].load() == 1U; }));
    std::uint64_t reacquired = 0U;
    V1_CHECK(wait_for(kWaitBound, [&]() {
        reacquired = monitor->acquire_lease(1U);
        return reacquired != 0U;
    }));
    const auto metadata = f.tuner->tune_metadata(1U);
    V1_CHECK(metadata && !metadata.value().current_valid &&
             metadata.value().last_successful.requested_hz == 473142857U);
    V1_CHECK(f.server_ok.load());
    return true;
}

// ---------------------------------------------------------------------------
// 4. Card operations still fail closed while typed stream requests reach the service.

bool test_pending_operations_fail_closed()
{
    Fixture f("v1-pending", true);
    V1_CHECK(f.start());
    auto client = TestClient::connect(f.endpoint);
    V1_CHECK(client && client->hello());
    const std::uint64_t lease = client->acquire_lease(1U);
    V1_CHECK(lease != 0U);
    Reply reply;
    V1_CHECK(client->tune(client->terrestrial(lease, 473142857U), reply));
    wire::TuneResult result;
    V1_CHECK(tune_result(reply, result));
    wire::StartRequest start;
    start.epoch = client->epoch;
    start.lease_id = lease;
    start.tune_revision = result.tune_revision;
    V1_CHECK(client->request(wire::kOpcodeStartStream,
                             TestClient::encode(start, &wire::encode_start_request), reply));
    const auto started = wire::decode_start_response(
        ByteView{reply.payload.data(), reply.payload.size()});
    V1_CHECK(started && started.value().epoch == client->epoch &&
             started.value().stream_id != 0U);
    wire::CardStatusRequest card;
    card.key.epoch = client->epoch;
    V1_CHECK(client->request(wire::kOpcodeCardStatus,
                             TestClient::encode(card, &wire::encode_card_status_request), reply));
    V1_CHECK(TestClient::error_code(reply) == static_cast<std::uint32_t>(Error::UNSUPPORTED));
    V1_CHECK(f.backend.capture_calls.load() == 0U && card_untouched(f));

    // Data endpoint: typed ATTACH is acknowledged; stale epochs are rejected.
    wire::AttachRequest attach;
    attach.epoch = client->epoch;
    attach.lease_id = lease;
    attach.stream_id = started.value().stream_id;
    attach.attach_token = started.value().attach_token;
    auto data = TestClient::connect(f.stream_endpoint);
    V1_CHECK(data);
    V1_CHECK(data->request(wire::kOpcodeAttachStream,
                           TestClient::encode(attach, &wire::encode_attach_request), reply));
    V1_CHECK(reply.header.kind == wire::Kind::response);
    data.reset();
    attach.epoch = make_epoch(0x55U);
    auto stale = TestClient::connect(f.stream_endpoint);
    V1_CHECK(stale);
    V1_CHECK(stale->request(wire::kOpcodeAttachStream,
                            TestClient::encode(attach, &wire::encode_attach_request), reply));
    V1_CHECK(TestClient::error_code(reply) == wire::kWireErrorStaleHandle);
    V1_CHECK(stale->closed_without_reply());
    auto legacy = TestClient::connect(f.stream_endpoint);
    V1_CHECK(legacy);
    auto bytes = TestClient::frame(wire::kOpcodeAttachStream, 1U, ByteView{nullptr, 0U});
    bytes[0] = 'P';
    bytes[1] = 'X';
    bytes[2] = '4';
    bytes[3] = 'U';
    V1_CHECK(legacy->send_bytes(bytes.data(), bytes.size()));
    V1_CHECK(legacy->closed_without_reply());
    V1_CHECK(f.backend.capture_calls.load() == 0U);
    V1_CHECK(f.server_ok.load());
    return true;
}

// ---------------------------------------------------------------------------
// 14. Card service over the control connection

bool test_card_service_over_control()
{
    Fixture f("v1-card");
    V1_CHECK(f.start());

    // CARD_SERVICE is refused with UNSUPPORTED until the client negotiates it,
    // and that refusal never reaches the card backend.
    {
        auto client = TestClient::connect(f.endpoint);
        V1_CHECK(client && client->hello());
        wire::CardStatusRequest card;
        card.key.epoch = client->epoch;
        card.key.reader_id = 0U;
        Reply reply;
        V1_CHECK(client->request(
            wire::kOpcodeCardStatus,
            TestClient::encode(card, &wire::encode_card_status_request), reply));
        V1_CHECK(TestClient::error_code(reply) ==
                 static_cast<std::uint32_t>(Error::UNSUPPORTED));
        V1_CHECK(card_untouched(f));
    }

    auto client = TestClient::connect(f.endpoint);
    V1_CHECK(client && client->hello(nullptr, wire::kFeatureCardService));

    // A reader id outside the canonical LIST is NOT_FOUND and powers nothing.
    {
        wire::CardStatusRequest card;
        card.key.epoch = client->epoch;
        card.key.reader_id = 0x1234U;
        Reply reply;
        V1_CHECK(client->request(
            wire::kOpcodeCardStatus,
            TestClient::encode(card, &wire::encode_card_status_request), reply));
        V1_CHECK(TestClient::error_code(reply) ==
                 static_cast<std::uint32_t>(Error::NOT_FOUND));
        V1_CHECK(card_untouched(f));
    }

    // The listed reader is observed through the card lane: the synthetic
    // backend reports a present but uninitialized card.
    {
        wire::CardStatusRequest card;
        card.key.epoch = client->epoch;
        card.key.reader_id = 0U;
        Reply reply;
        V1_CHECK(client->request(
            wire::kOpcodeCardStatus,
            TestClient::encode(card, &wire::encode_card_status_request), reply));
        V1_CHECK(reply.header.kind == wire::Kind::response);
        const auto decoded = wire::decode_card_status_response(reply.view());
        V1_CHECK(decoded);
        V1_CHECK(decoded.value().key.reader_id == 0U);
        V1_CHECK(decoded.value().current_generation != 0U);
        V1_CHECK(decoded.value().present && !decoded.value().initialized);
        V1_CHECK(!decoded.value().protocol.present && !decoded.value().atr.present);
        V1_CHECK(f.card_backend.detect_calls.load() != 0U);
    }

    // CONNECT reaches the lane and returns a bound handle.  The synthetic
    // session exposes no ATR, so the handle is reported not usable rather than
    // carrying a fabricated ATR.
    std::uint64_t handle = 0U;
    {
        wire::CardConnectRequest connect;
        connect.key.epoch = client->epoch;
        connect.key.reader_id = 0U;
        connect.share = wire::Share::shared;
        connect.preferred_protocols = 1U;
        Reply reply;
        V1_CHECK(client->request(
            wire::kOpcodeCardConnect,
            TestClient::encode(connect, &wire::encode_card_connect_request), reply));
        V1_CHECK(reply.header.kind == wire::Kind::response);
        const auto decoded = wire::decode_card_session_result(reply.view());
        V1_CHECK(decoded && decoded.value().card_handle != 0U);
        V1_CHECK(decoded.value().reader_id == 0U);
        V1_CHECK(!decoded.value().usable);
        handle = decoded.value().card_handle;
    }

    // A live handle presented with the wrong stored generation is STALE_HANDLE
    // (the wire code 16), and the APDU never reaches the session.
    {
        wire::CardTransmitRequest transmit;
        transmit.key.epoch = client->epoch;
        transmit.key.reader_id = 0U;
        transmit.key.bound_generation = 2U;
        transmit.key.card_handle = handle;
        transmit.command.length = 1U;
        transmit.command.bytes[0] = 0x00U;
        Reply reply;
        V1_CHECK(client->request(
            wire::kOpcodeCardTransmit,
            TestClient::encode(transmit, &wire::encode_card_transmit_request), reply));
        V1_CHECK(TestClient::error_code(reply) == wire::kWireErrorStaleHandle);
        V1_CHECK(f.card_session.transmit_calls.load() == 0U);
    }

    // DISCONNECT consumes the handle even though the session has no ATR.
    {
        wire::CardDisconnectRequest disconnect;
        disconnect.key.epoch = client->epoch;
        disconnect.key.reader_id = 0U;
        disconnect.key.bound_generation = 1U;
        disconnect.key.card_handle = handle;
        disconnect.disposition = wire::Disposition::leave;
        Reply reply;
        V1_CHECK(client->request(
            wire::kOpcodeCardDisconnect,
            TestClient::encode(disconnect, &wire::encode_card_disconnect_request), reply));
        V1_CHECK(reply.header.kind == wire::Kind::response);
        const auto decoded = wire::decode_card_disconnect_response(reply.view());
        V1_CHECK(decoded && decoded.value().consumed && decoded.value().quiesced);
    }

    // The consumed handle is now unknown to this owner.
    {
        wire::CardHandleStatusRequest status;
        status.key.epoch = client->epoch;
        status.key.reader_id = 0U;
        status.key.bound_generation = 1U;
        status.key.card_handle = handle;
        Reply reply;
        V1_CHECK(client->request(
            wire::kOpcodeCardHandleStatus,
            TestClient::encode(status, &wire::encode_card_handle_status_request), reply));
        V1_CHECK(TestClient::error_code(reply) ==
                 static_cast<std::uint32_t>(Error::NOT_FOUND));
    }

    V1_CHECK(f.server_ok.load());
    return true;
}

// ---------------------------------------------------------------------------
// Separate process: a fresh epoch per daemon context, and a restarted daemon
// rejects the previous epoch.

volatile std::sig_atomic_t g_child_stop = 0;

void on_child_term(int) { g_child_stop = 1; }

[[noreturn]] void run_child_server(const std::string& runtime, int ready_fd)
{
    std::signal(SIGTERM, on_child_term);
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    Backend backend;
    PosixTunerNonceSource nonce;
    Clock clock;
    TunerService tuner(backend, nonce, clock);
    std::unique_ptr<ControlContext> context(new ControlContext());
    ClaimedUsbFunctionObservation first;
    ClaimedUsbFunctionObservation second;
    const auto epoch = nonce.generate();
    int status = 1;
    if (epoch && tuner.configure_strict_capabilities() &&
        product_context(tuner, epoch.value(), *context, first, second)) {
        const EndpointConfig endpoint{runtime.c_str(), "v1-process", kControlEndpointName,
                                      EndpointAccess::private_user};
        auto server = PosixControlServer::create(endpoint, card, tuner, *context);
        if (server) {
            const char ready = 'R';
            (void)!::write(ready_fd, &ready, 1U);
            status = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (g_child_stop == 0 && std::chrono::steady_clock::now() < deadline) {
                if (!server.value()->poll_once(Timeout{10U})) {
                    status = 1;
                    break;
                }
            }
            (void)server.value()->shutdown();
        }
    }
    (void)card.shutdown();
    ::close(ready_fd);
    std::_Exit(status);
}

// Starts the child and waits (bounded) for its ready byte.
pid_t start_child(const std::string& runtime)
{
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) return -1;
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(pipe_fds[0]);
        run_child_server(runtime, pipe_fds[1]);
    }
    ::close(pipe_fds[1]);
    if (pid < 0) {
        ::close(pipe_fds[0]);
        return -1;
    }
    pollfd descriptor{pipe_fds[0], POLLIN, 0};
    char ready = 0;
    const bool ok = ::poll(&descriptor, 1U, 10000) == 1 && ::read(pipe_fds[0], &ready, 1U) == 1 &&
                    ready == 'R';
    ::close(pipe_fds[0]);
    if (!ok) {
        ::kill(pid, SIGKILL);
        (void)::waitpid(pid, nullptr, 0);
        return -1;
    }
    return pid;
}

// Stops the child with SIGTERM, escalating to SIGKILL after a bound.
bool stop_child(pid_t pid)
{
    if (pid <= 0) return false;
    ::kill(pid, SIGTERM);
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ::kill(pid, SIGKILL);
    (void)::waitpid(pid, &status, 0);
    return false;
}

[[maybe_unused]] bool test_fresh_epoch_after_restart()
{
    TempRuntime runtime;
    V1_CHECK(runtime.valid());
    const EndpointConfig endpoint{runtime.path().c_str(), "v1-process", kControlEndpointName,
                                  EndpointAccess::private_user};
    const pid_t first_pid = start_child(runtime.path());
    V1_CHECK(first_pid > 0);
    wire::Uuid first_epoch{};
    {
        auto client = TestClient::connect(endpoint);
        const bool ok = client && client->hello();
        if (ok) first_epoch = client->epoch;
        if (!ok) {
            (void)stop_child(first_pid);
            return false;
        }
    }
    V1_CHECK(stop_child(first_pid));
    const pid_t second_pid = start_child(runtime.path());
    V1_CHECK(second_pid > 0);
    bool ok = false;
    {
        auto client = TestClient::connect(endpoint);
        if (client && client->hello() && client->epoch != first_epoch) {
            wire::ListRequest list;
            list.epoch = first_epoch;
            Reply reply;
            ok = client->request(wire::kOpcodeList,
                                 TestClient::encode(list, &wire::encode_list_request), reply) &&
                 TestClient::error_code(reply) == wire::kWireErrorStaleHandle;
            wire::ListResponse fresh;
            ok = ok && client->list(fresh) && fresh.epoch == client->epoch;
        }
    }
    const bool stopped = stop_child(second_pid);
    V1_CHECK(ok && stopped);
    return true;
}

}  // namespace

int main()
{
    struct Case final {
        const char* name;
        bool (*function)();
    };
    const Case cases[] = {
        {"normal_recording_file_output", test_normal_recording_file_output},
        {"normal_recording_duration_without_ts",
         test_normal_recording_duration_without_ts},
        {"product_device_only_usb_recording", test_product_device_only_usb_recording},
        {"product_device_instance_recording", test_product_device_instance_recording},
        {"product_instance_only_recording", test_product_instance_only_recording},
        {"product_device_instance_mismatch_no_acquire",
         test_product_device_instance_mismatch_no_acquire},
        {"product_serial_absence_not_found", test_product_serial_absence_not_found},
        {"framing_and_handshake", test_framing_and_handshake},
        {"partial_response_write", test_partial_response_write},
        {"asicen_context_profile_facts", test_asicen_context_profile_facts},
        {"mock_factory_context_reaches_server", test_mock_factory_context_reaches_server},
        {"list_status_one_context", test_list_status_one_context},
        {"tune_exact_hz_and_release", test_tune_exact_hz_and_release},
        {"unnegotiated_pid_filter_stops_before_backend",
         test_unnegotiated_pid_filter_stops_before_backend},
        {"asicen_same_lane_exclusion", test_asicen_same_lane_exclusion},
        {"request_stop_precedes_worker_drain", test_request_stop_precedes_worker_drain},
        {"owner_eof_during_tune_and_blocked_cleanup",
         test_owner_eof_during_tune_and_blocked_cleanup},
        {"pending_operations_fail_closed", test_pending_operations_fail_closed},
        {"card_service_over_control", test_card_service_over_control},
        {"fresh_epoch_after_restart", test_fresh_epoch_after_restart},
    };
    int failures = 0;
    for (const Case& test : cases) {
        const bool passed = test.function();
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", test.name);
        if (!passed) ++failures;
    }
    return failures == 0 ? 0 : 1;
}
