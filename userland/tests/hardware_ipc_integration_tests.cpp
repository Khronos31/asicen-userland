// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_stream_session.h"
#include "asicen/px4_mock_backend.h"
#include "dtv/userland/control_client.h"
#include "dtv/userland/host_listener_context/enclosure_context.h"
#include "dtv/userland/host_runtime/control_socket.h"
#include "dtv/userland/wire.h"
#include "test_temp_directory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#if defined(__linux__)
#include <pthread.h>
#endif
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void report_current_stack(const char* label) noexcept
{
#if defined(__linux__)
    pthread_attr_t attributes{};
    if (::pthread_getattr_np(::pthread_self(), &attributes) == 0) {
        void* base = nullptr;
        std::size_t size = 0U;
        if (::pthread_attr_getstack(&attributes, &base, &size) == 0)
            std::fprintf(stderr, "%s stack-size=%zu\n", label, size);
        (void)::pthread_attr_destroy(&attributes);
    }
#else
    (void)label;
#endif
}
namespace w = dtv::userland::wire;
using namespace dtv::userland;
using namespace dtv::userland::host_listener_context;
using namespace dtv::userland::host_runtime;
using namespace px4::userland;

template <std::size_t N>
w::WString<N> text(const char* value)
{
    w::WString<N> result;
    while (value[result.length] != '\0') {
        result.bytes[result.length] = value[result.length];
        ++result.length;
    }
    return result;
}

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }
    void sleep_ms(std::uint32_t ms) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
};

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, ipc::kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, ipc::kNonceLength> value{};
        value[0U] = 1U;
        return Result<std::array<std::uint8_t, ipc::kNonceLength>>::success(value);
    }
};

class Source final : public asicen::StreamCaptureSource {
public:
    Result<void> prepare(std::uint8_t receiver, ipc::System system,
                         const std::atomic<bool>& cancelled) noexcept override
    {
        return receiver == 1U && system == ipc::System::ISDB_T && !cancelled.load()
                   ? Result<void>::success()
                   : Result<void>::failure(Error::UNSUPPORTED);
    }
    asicen::CaptureRunResult run(const std::atomic<bool>& cancelled,
                                 bool (*emit)(void*, const std::uint8_t*, std::size_t),
                                 void* context) noexcept override
    {
        std::array<std::uint8_t, 188U> packet{};
        packet[0U] = 0x47U;
        packet[1U] = 0U;
        packet[2U] = 0x20U;
        packet[3U] = 0x10U;
        if (!cancelled.load()) (void)emit(context, packet.data(), packet.size());
        std::unique_lock<std::mutex> lock(mutex_);
        run_started_.store(true, std::memory_order_release);
        condition_.notify_all();
        condition_.wait(lock, [&] { return cancelled.load() || interrupted_; });
        if (hold_after_interrupt_) condition_.wait(lock, [&] { return release_run_; });
        return asicen::CaptureRunResult::cancelled;
    }
    void interrupt() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            interrupted_ = true;
            interrupt_seen_.store(true, std::memory_order_release);
        }
        condition_.notify_all();
    }
    Result<void> stop() noexcept override { return Result<void>::success(); }
    void hold_after_interrupt() noexcept { hold_after_interrupt_ = true; }
    void release_run() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            release_run_ = true;
        }
        condition_.notify_all();
    }
    bool interrupt_seen() const noexcept { return interrupt_seen_.load(std::memory_order_acquire); }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool interrupted_ = false;
    bool hold_after_interrupt_ = false;
    bool release_run_ = false;
    std::atomic<bool> run_started_{false};
    std::atomic<bool> interrupt_seen_{false};
};

class Family final : public EnclosureFamily {
public:
    Family() : frontend_(4U), stream_(frontend_, source_) {}
    TunerServiceBackend& tuner_backend() noexcept override { return stream_; }
    CardServiceBackend& card_backend() noexcept override { return card_backend_; }
    CardProtocolSession& card_protocol() noexcept override { return card_session_; }
    TunerServiceTime& time() noexcept override { return time_; }
    TunerStreamControl* stream_control() noexcept override { return &stream_; }
    Source& source() noexcept { return source_; }

private:
    asicen::MockTunerBackend frontend_;
    Source source_;
    asicen::HardwareStreamService stream_;
    asicen::UnsupportedCardBackend card_backend_;
    asicen::UnsupportedCardSession card_session_;
    Time time_;
};

w::ListResponse make_list(const w::Uuid& epoch)
{
    w::ListResponse list;
    list.epoch = epoch;
    list.producer.product = text<32U>("asicen-userland");
    list.producer.version = text<64U>("test");
    list.topology.state = w::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.functions.count = 1U;
    auto& function = list.functions.items[0U];
    function.id = 1U;
    function.vid = 0x0b06U;
    function.pid = 0x0005U;
    function.mode = w::Mode::runtime;
    function.serial_state = w::SerialState::absent;
    function.identification_reason = w::IdentificationReason::descriptor_unreadable;
    function.present = true;
    list.enclosure.present = true;
    list.enclosure.value.id = 0U;
    list.enclosure.value.identity.serial_state = w::IdentitySerialState::absent;
    list.receivers.present = true;
    list.receivers.value.count = 1U;
    auto& receiver = list.receivers.value.items[0U];
    receiver.id = 1U;
    receiver.systems.mask = 1U;
    receiver.binding.present = true;
    receiver.binding.value.function_id = 1U;
    list.readers.present = true;
    list.readers.value.count = 1U;
    list.readers.value.items[0U].id = 0U;
    return list;
}

bool read_frame(w::TimedClientTransport& transport, w::Frame& frame,
                std::uint32_t timeout_ms = 2000U) noexcept
{
    std::array<std::uint8_t, w::kHeaderSize> header_bytes{};
    std::size_t offset = 0U;
    while (offset < header_bytes.size()) {
        const auto read = transport.read_some({header_bytes.data() + offset,
                                               header_bytes.size() - offset}, Timeout{timeout_ms});
        if (!read || read.value() == 0U) return false;
        offset += read.value();
    }
    const auto header = w::decode_header({header_bytes.data(), header_bytes.size()});
    if (!header || header.value().payload_length > w::kMaxTsDataPayload) return false;
    std::array<std::uint8_t, w::kMaxTsDataPayload> bytes{};
    offset = 0U;
    while (offset < header.value().payload_length) {
        const auto read = transport.read_some({bytes.data() + offset,
                                               header.value().payload_length - offset},
                                              Timeout{timeout_ms});
        if (!read || read.value() == 0U) return false;
        offset += read.value();
    }
    std::array<std::uint8_t, w::kHeaderSize + w::kMaxTsDataPayload> frame_bytes{};
    std::memcpy(frame_bytes.data(), header_bytes.data(), header_bytes.size());
    std::memcpy(frame_bytes.data() + header_bytes.size(), bytes.data(), offset);
    const auto decoded = w::decode_frame({frame_bytes.data(), header_bytes.size() + offset});
    if (!decoded) return false;
    frame = decoded.value();
    return true;
}

bool counter_value(const w::StreamCounters& counters, std::uint8_t id,
                   std::uint64_t expected) noexcept
{
    for (std::size_t i = 0U; i < counters.entries.count; ++i) {
        const auto& entry = counters.entries.items[i];
        if (entry.id == id) {
            return entry.counter.measurement.state == w::MeasureState::valid &&
                   entry.counter.measurement.value == expected;
        }
    }
    return false;
}

bool run(bool hold_detach = false)
{
    std::fprintf(stderr, "hw-ipc: start hold=%d\n", hold_detach);
    std::string pattern = test::temporary_directory_template("asicen-hw-ipc-");
    char* made = ::mkdtemp(pattern.data());
    if (made == nullptr) return false;
    const std::string root(made);
    std::fprintf(stderr, "hw-ipc: directory\n");
    std::error_code fs_error;
    if (::chmod(root.c_str(), 0700) != 0) {
        std::filesystem::remove_all(root, fs_error);
        return false;
    }

    std::unique_ptr<Family> family(new Family());
    if (hold_detach) family->source().hold_after_interrupt();
    Source* source = &family->source();
    ContextConfig config;
    config.runtime_directory = root;
    config.product = "asicen-userland";
    config.instance = "receiver1";
    config.server.epoch[0U] = 1U;
    config.server.features = w::kFeatureCardService;
    config.canonical_list = make_list(config.server.epoch);
    config.server.producer = config.canonical_list.producer;
    config.context.epoch = config.server.epoch;
    config.context.state = w::ContextState::ready;
    config.stream_engine_epoch = 0x41534943454eULL;
    config.stream_arm_budget_ms = 5000U;
    std::unique_ptr<EnclosureFamily> owned_family(family.release());
    auto assembled = EnclosureContext::assemble(std::move(owned_family),
        std::unique_ptr<TunerNonceSource>(new Nonce()), config);
    std::fprintf(stderr, "hw-ipc: assembled=%d\n", static_cast<bool>(assembled));
    if (!assembled || !assembled.value()->reserve_namespace() || !assembled.value()->publish()) {
        std::filesystem::remove_all(root, fs_error);
        return false;
    }
    auto context = std::move(assembled.value());
    std::atomic<bool> serving{true};
    std::thread poller([&] {
        std::fprintf(stderr, "hw-ipc: poll-start\n");
        while (serving.load(std::memory_order_acquire)) (void)context->poll_once(Timeout{10U});
    });

    const EndpointConfig control_endpoint{root, "asicen-userland", "receiver1"};
    const char* stage = "control-connect";
    const bool passed = [&]() {
        std::fprintf(stderr, "hw-ipc: control-connect\n");
        auto control_result = connect_control(control_endpoint, Timeout{2000U});
        std::fprintf(stderr, "hw-ipc: control-connected=%d\n", static_cast<bool>(control_result));
        if (!control_result) return false;
        auto control = std::move(control_result.value());
        w::RecordingOptions options;
        options.receiver_id = 1U;
        options.tune.epoch = config.server.epoch;
        options.tune.system = w::System::isdb_t;
        options.tune.frequency_hz = 557142000U;
        options.tune.timeout_ms = 1000U;
        report_current_stack("hw-ipc: caller");
        stage = "begin-recording";
        auto recording = w::begin_recording(*control, options, Timeout{5000U});
        std::fprintf(stderr, "hw-ipc: recording=%d\n", static_cast<bool>(recording));
        if (!recording) return false;

        const EndpointConfig data_endpoint{root, "asicen-userland", "receiver1",
                                           EndpointAccess::private_user, -1, "stream.sock"};
        stage = "connect-stream-socket";
        auto data_result = connect_endpoint(data_endpoint, Timeout{2000U});
        std::fprintf(stderr, "hw-ipc: stream-connected=%d\n", static_cast<bool>(data_result));
        if (!data_result) return false;
        auto data = std::move(data_result.value());
        w::AttachRequest attach;
        attach.epoch = recording.session.epoch;
        attach.lease_id = recording.session.lease_id;
        attach.stream_id = recording.session.stream_id;
        attach.attach_token = recording.session.attach_token;
        std::array<std::uint8_t, w::kHeaderSize + w::kMaxControlPayload> attach_frame{};
        const auto attach_size = w::encode_record_frame(w::Kind::request,
            w::kOpcodeAttachStream, 1U, attach, &w::encode_attach_request,
            {attach_frame.data(), attach_frame.size()});
        stage = "attach-write";
        if (!attach_size || !data->write_all({attach_frame.data(), attach_size.value()},
                                              Timeout{1000U})) return false;

        w::Frame frame;
        stage = "attach-response";
        if (!read_frame(*data, frame) || frame.header.kind != w::Kind::response ||
            frame.header.opcode != w::kOpcodeAttachStream ||
            !w::decode_attach_response(frame.payload)) return false;
        std::fprintf(stderr, "hw-ipc: attach-response\n");
        stage = "ts-data";
        if (!read_frame(*data, frame) || frame.header.kind != w::Kind::event ||
            frame.header.opcode != w::kOpcodeTsDataEvent) return false;
        std::fprintf(stderr, "hw-ipc: ts-data\n");
        const auto ts = w::decode_ts_data(frame.payload);
        if (!ts || ts.value().packets.size != 188U || ts.value().packets.data[0U] != 0x47U)
            return false;

        stage = "stop-stream";
        w::StopControlResult stop;
        if (hold_detach) {
            std::atomic<bool> stop_done{false};
            std::thread stopper([&] {
                stop = w::stop_recording(*control, recording.session, Timeout{5000U});
                stop_done.store(true, std::memory_order_release);
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!source->interrupt_seen() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!source->interrupt_seen()) {
                std::fprintf(stderr, "STOP did not interrupt producer\n");
                source->release_run();
                stopper.join();
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            w::Frame premature_end;
            const bool ended_before_release = read_frame(*data, premature_end, 50U);
            source->release_run();
            stopper.join();
            if (ended_before_release) {
                std::fprintf(stderr, "STREAM_END arrived before producer release\n");
                return false;
            }
            if (!stop_done.load(std::memory_order_acquire)) return false;
        } else {
            std::fprintf(stderr, "hw-ipc: stopping\n");
            stop = w::stop_recording(*control, recording.session, Timeout{5000U});
        }
        std::fprintf(stderr, "hw-ipc: stopped=%d\n", static_cast<bool>(stop));
        if (!stop) {
            std::fprintf(stderr, "STOP error=%u remote=%u\n",
                         static_cast<unsigned>(stop.error), stop.remote_error_code);
            return false;
        }
        stage = "stream-end";
        if (!read_frame(*data, frame)) return false;
        if (frame.header.kind != w::Kind::event ||
            frame.header.opcode != w::kOpcodeStreamEndEvent) {
            std::fprintf(stderr, "unexpected terminal frame kind=%u opcode=%u\n",
                         static_cast<unsigned>(frame.header.kind), frame.header.opcode);
            return false;
        }
        const auto end = w::decode_stream_end_event(frame.payload);
        if (!end || end.value().final.id != recording.session.stream_id ||
            !counter_value(end.value().final.statistics, w::kCounterIdTransmittedPackets, 1U) ||
            !counter_value(end.value().final.statistics, w::kCounterIdQueueDroppedPackets, 0U)) {
            std::fprintf(stderr, "stream end mismatch decode=%u id=%llu expected=%llu counters=%u/%u\n",
                         static_cast<unsigned>(end.error()),
                         static_cast<unsigned long long>(end ? end.value().final.id : 0U),
                         static_cast<unsigned long long>(recording.session.stream_id),
                         static_cast<unsigned>(end ? end.value().final.statistics.entries.count : 0U),
                         static_cast<unsigned>(stop.response.quiesced));
            return false;
        }
        data->close();
        const auto released = w::release_recording(*control, recording.session, Timeout{2000U});
        if (!released) return false;
        control->close();
        return true;
    }();
    if (!passed) std::fprintf(stderr, "hardware IPC integration failed at %s\n", stage);
    serving.store(false, std::memory_order_release);
    poller.join();
    (void)context->close(Timeout{2000U});
    context.reset();
    std::filesystem::remove_all(root, fs_error);
    return passed;
}
}  // namespace

int main()
{
    if (!run()) return 1;
    if (!run(true)) return 1;
    return 0;
}
