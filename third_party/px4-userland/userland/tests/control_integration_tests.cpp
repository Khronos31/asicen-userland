// SPDX-License-Identifier: GPL-2.0-only
#include "px4/card_service.h"
#include "px4/control_client.h"
#include "px4/control_server.h"
#include "px4/tuner_service.h"
#include "card_client_contract_tests.h"
#include "control_server_test_access.h"
#include "control_workers.h"
#include "test_temp_directory.h"
#include "asicen/control_context_builder.h"
#include "asicen/device_profile.h"
#include <dtv/userland/records.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>
#include <map>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;
namespace wire = dtv::userland::wire;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

class TempRuntime final {
public:
    TempRuntime()
    {
        std::string pattern = test::temporary_directory_template("px4-control-");
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

struct WireReply final {
    wire::Header header{};
    std::vector<std::uint8_t> payload;
    ByteView view() const noexcept { return ByteView{payload.data(), payload.size()}; }
};

class WireCollector final : public wire::FrameHandler {
public:
    Result<void> on_frame(const wire::Frame& frame) noexcept override
    {
        WireReply reply;
        reply.header = frame.header;
        reply.payload.assign(frame.payload.data, frame.payload.data + frame.payload.size);
        frames.push_back(std::move(reply));
        return Result<void>::success();
    }
    std::vector<WireReply> frames;
};

class WireClient final {
public:
    explicit WireClient(SocketStream stream)
        : stream_(std::move(stream)),
          storage_(new std::array<std::uint8_t, wire::kMaxControlPayload>()),
          receiver_(MutableByteView{storage_->data(), storage_->size()})
    {
    }

    static std::unique_ptr<WireClient> connect(const EndpointConfig& endpoint)
    {
        auto stream = SocketStream::connect(endpoint, Timeout{2000U});
        if (!stream) return nullptr;
        return std::unique_ptr<WireClient>(new WireClient(std::move(stream.value())));
    }

    bool send_bytes(const std::uint8_t* data, std::size_t size)
    {
        std::size_t sent = 0U;
        while (sent < size) {
            const ssize_t count = ::send(stream_.native_handle(), data + sent,
                                         size - sent, MSG_NOSIGNAL);
            if (count <= 0) return false;
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    static std::vector<std::uint8_t> frame(std::uint16_t opcode,
                                            std::uint32_t request_id,
                                            ByteView payload,
                                            wire::Kind kind = wire::Kind::request)
    {
        std::vector<std::uint8_t> bytes(wire::kHeaderSize + payload.size);
        wire::Header header;
        header.kind = kind;
        header.opcode = opcode;
        header.request_id = request_id;
        header.payload_length = static_cast<std::uint32_t>(payload.size);
        const auto written = wire::encode_frame(
            header, payload, MutableByteView{bytes.data(), bytes.size()});
        if (!written) bytes.clear();
        return bytes;
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

    bool read_frame(WireReply& out, std::chrono::milliseconds bound =
                    std::chrono::seconds(5))
    {
        const auto deadline = std::chrono::steady_clock::now() + bound;
        while (collector_.frames.empty()) {
            if (std::chrono::steady_clock::now() >= deadline || closed_) return false;
            std::array<std::uint8_t, 4096U> buffer{};
            const auto read = stream_.read_some(
                MutableByteView{buffer.data(), buffer.size()}, Timeout{20U});
            if (!read) {
                if (read.error() == Error::DISCONNECTED) closed_ = true;
                continue;
            }
            if (!receiver_.feed(ByteView{buffer.data(), read.value()}, collector_))
                return false;
        }
        out = std::move(collector_.frames.front());
        collector_.frames.erase(collector_.frames.begin());
        return true;
    }

    bool closed_without_reply(std::chrono::milliseconds bound =
                              std::chrono::seconds(5))
    {
        WireReply reply;
        if (read_frame(reply, bound)) return false;
        return closed_;
    }

    bool request(std::uint16_t opcode, const std::vector<std::uint8_t>& payload,
                 WireReply& out)
    {
        const auto bytes = frame(opcode, ++request_id_,
                                 ByteView{payload.data(), payload.size()});
        if (bytes.empty() || !send_bytes(bytes.data(), bytes.size())) return false;
        return read_frame(out) && out.header.request_id == request_id_ &&
               out.header.opcode == opcode;
    }

    bool hello(std::uint64_t required, std::uint64_t optional,
               wire::HelloResponse* result = nullptr)
    {
        wire::HelloRequest value;
        value.min = wire::Version{1U, 0U};
        value.max = wire::Version{1U, 0U};
        value.required_features = required;
        value.optional_features = optional;
        WireReply reply;
        if (!request(wire::kOpcodeHello,
                     encode(value, &wire::encode_hello_request), reply) ||
            reply.header.kind != wire::Kind::response) return false;
        const auto decoded = wire::decode_hello_response(reply.view());
        if (!decoded) return false;
        epoch = decoded.value().epoch;
        features = decoded.value().features;
        if (result != nullptr) *result = decoded.value();
        return true;
    }

    bool send_request(std::uint16_t opcode, const std::vector<std::uint8_t>& payload)
    {
        const auto bytes = frame(opcode, ++request_id_,
                                 ByteView{payload.data(), payload.size()});
        return !bytes.empty() && send_bytes(bytes.data(), bytes.size());
    }

    void close() { stream_.close(); }
    int fd() const noexcept { return stream_.native_handle(); }
    std::uint32_t request_id() const noexcept { return request_id_; }

    wire::Uuid epoch{};
    std::uint64_t features = 0U;

private:
    SocketStream stream_;
    std::unique_ptr<std::array<std::uint8_t, wire::kMaxControlPayload>> storage_;
    wire::FrameReceiver receiver_;
    WireCollector collector_;
    std::uint32_t request_id_ = 0U;
    bool closed_ = false;
};

class Backend final : public CardServiceBackend {
public:
    Result<void> set_power(bool on) noexcept override
    {
        power_calls.push_back(on);
        powered = on;
        return Result<void>::success();
    }
    Result<void> initialize_uart() noexcept override
    {
        ++uart_calls;
        return Result<void>::success();
    }
    Result<bool> detect_card() noexcept override
    {
        return Result<bool>::success(present.load());
    }
    std::vector<bool> power_calls;
    std::size_t uart_calls = 0U;
    bool powered = false;
    std::atomic<bool> present{true};
};

class TunerBackend final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override { return kReceiverCount; }
    bool receiver_supports(std::uint8_t receiver, System system) const noexcept override
    {
        return receiver < kReceiverCount &&
               system == (receiver % 4U < 2U ? System::ISDB_S : System::ISDB_T);
    }
    TunerStrictProfile strict_profile(std::uint8_t receiver,
                                      System system) const noexcept override
    {
        if (!receiver_supports(receiver, system)) return {};
        TunerStrictProfile profile{};
        profile.native_hz = true;
        profile.min_hz = system == System::ISDB_T ? 40000000U : 950000000U;
        profile.max_hz = system == System::ISDB_T ? 900000000U : 2150000000U;
        if (system == System::ISDB_S) {
            profile.lnb_voltage_mask = 0x03U;
            profile.slot_count = 8U;
        }
        return profile;
    }
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        ++open_calls[receiver];
        if (receiver == 0U && block_receiver_zero.load()) {
            {
                std::lock_guard<std::mutex> lock(receiver_zero_mutex);
                receiver_zero_started.store(true);
            }
            receiver_zero_condition.notify_all();
            while (block_receiver_zero.load()) std::this_thread::yield();
        }
        if (open_error != Error::OK) return Result<void>::failure(open_error);
        return Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t receiver, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        if (receiver == 0U && block_tune_receiver_zero.load()) {
            {
                std::lock_guard<std::mutex> lock(tune_receiver_zero_mutex);
                tune_receiver_zero_started.store(true);
            }
            tune_receiver_zero_condition.notify_all();
            while (block_tune_receiver_zero.load()) std::this_thread::yield();
        }
        last_system[receiver] = System::ISDB_T;
        ++tune_calls[receiver];
        return Result<void>::success();
    }
    Result<void> tune_satellite(std::uint8_t receiver, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        if (receiver == 0U && block_tune_receiver_zero.load()) {
            {
                std::lock_guard<std::mutex> lock(tune_receiver_zero_mutex);
                tune_receiver_zero_started.store(true);
            }
            tune_receiver_zero_condition.notify_all();
            while (block_tune_receiver_zero.load()) std::this_thread::yield();
        }
        last_system[receiver] = System::ISDB_S;
        ++tune_calls[receiver];
        return Result<void>::success();
    }
    Result<TunerHzProgramming> tune_terrestrial_hz(
        std::uint8_t receiver, std::uint64_t frequency_hz,
        std::uint32_t) noexcept override
    {
        if (receiver == 0U && block_tune_receiver_zero.load()) {
            {
                std::lock_guard<std::mutex> lock(tune_receiver_zero_mutex);
                tune_receiver_zero_started.store(true);
            }
            tune_receiver_zero_condition.notify_all();
            while (block_tune_receiver_zero.load()) std::this_thread::yield();
        }
        last_system[receiver] = System::ISDB_T;
        last_frequency_hz[receiver] = frequency_hz;
        ++tune_calls[receiver];
        TunerHzProgramming result{};
        result.programmed_hz.present = true;
        result.programmed_hz.value = frequency_hz;
        return Result<TunerHzProgramming>::success(result);
    }
    Result<TunerHzProgramming> tune_satellite_hz(
        std::uint8_t receiver, std::uint64_t frequency_hz,
        std::uint32_t) noexcept override
    {
        if (receiver == 0U && block_tune_receiver_zero.load()) {
            {
                std::lock_guard<std::mutex> lock(tune_receiver_zero_mutex);
                tune_receiver_zero_started.store(true);
            }
            tune_receiver_zero_condition.notify_all();
            while (block_tune_receiver_zero.load()) std::this_thread::yield();
        }
        last_system[receiver] = System::ISDB_S;
        last_frequency_hz[receiver] = frequency_hz;
        ++tune_calls[receiver];
        TunerHzProgramming result{};
        result.programmed_hz.present = true;
        result.programmed_hz.value = frequency_hz;
        return Result<TunerHzProgramming>::success(result);
    }
    Result<bool> is_locked(std::uint8_t, System) noexcept override
    { return Result<bool>::success(true); }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    {
        ++close_calls[receiver];
        return Result<void>::success();
    }

    Result<void> begin_tune_power(std::uint8_t, System system,
                                  std::uint8_t voltage) noexcept override
    {
        if (system == System::ISDB_S && voltage == 15U) {
            if (!allow_lnb_15v.load())
                return Result<void>::failure(Error::UNSUPPORTED);
            ++lnb_gpio_calls;
        }
        return Result<void>::success();
    }

    Result<void> start_capture(std::uint8_t receiver, System system) noexcept override
    {
        ++start_capture_calls[receiver];
        last_capture_system[receiver] = system;
        if (receiver == 0U && block_start_capture.load()) {
            std::unique_lock<std::mutex> lock(start_capture_mutex);
            start_capture_started.store(true);
            start_capture_condition.notify_all();
            start_capture_condition.wait(lock, [&]() noexcept {
                return !block_start_capture.load();
            });
        }
        return Result<void>::success();
    }

    Result<void> stop_capture(std::uint8_t receiver, System system) noexcept override
    {
        ++stop_capture_calls[receiver];
        last_capture_system[receiver] = system;
        return Result<void>::success();
    }

    Error open_error = Error::OK;
    std::atomic<bool> allow_lnb_15v{false};
    std::atomic<std::size_t> lnb_gpio_calls{0U};
    std::array<std::size_t, kReceiverCount> open_calls{};
    std::array<std::size_t, kReceiverCount> tune_calls{};
    std::array<std::size_t, kReceiverCount> close_calls{};
    std::array<std::size_t, kReceiverCount> start_capture_calls{};
    std::array<std::size_t, kReceiverCount> stop_capture_calls{};
    std::array<System, kReceiverCount> last_system{};
    std::array<System, kReceiverCount> last_capture_system{};
    std::array<std::uint64_t, kReceiverCount> last_frequency_hz{};
    std::atomic<bool> block_receiver_zero{false};
    std::atomic<bool> receiver_zero_started{false};
    std::mutex receiver_zero_mutex;
    std::condition_variable receiver_zero_condition;
    std::atomic<bool> block_tune_receiver_zero{false};
    std::atomic<bool> tune_receiver_zero_started{false};
    std::mutex tune_receiver_zero_mutex;
    std::condition_variable tune_receiver_zero_condition;
    std::atomic<bool> block_start_capture{false};
    std::atomic<bool> start_capture_started{false};
    std::mutex start_capture_mutex;
    std::condition_variable start_capture_condition;
};

bool make_control_context(TunerService& tuner_service, const char* instance,
                          std::uint8_t epoch_seed, ControlContext& context)
{
    if (!tuner_service.configure_strict_capabilities()) return false;
    const auto* profile = asicen::find_profile(asicen::ModelId::W3u3);
    if (profile == nullptr) return false;
    std::array<asicen::ClaimedUsbFunctionObservation, 2U> observations{};
    for (std::size_t index = 0U; index < observations.size(); ++index) {
        observations[index].vendor_id = profile->vid;
        observations[index].product_id = profile->pid;
        observations[index].has_bus = true;
        observations[index].bus = 3U;
        observations[index].has_address = true;
        observations[index].address = static_cast<std::uint8_t>(11U + index);
        observations[index].port_path = {1U, static_cast<std::uint8_t>(index + 1U)};
    }
    wire::Uuid epoch{};
    for (std::size_t index = 0U; index < epoch.size(); ++index)
        epoch[index] = static_cast<std::uint8_t>(epoch_seed + index * 7U);
    if (!asicen::build_asicen_control_context(
        *profile, observations.data(), 2U, 2U, tuner_service, instance, epoch, context))
        return false;
    // This portable socket fixture represents its eight-receiver fake, not a
    // physical W3U3. Preserve the independent bridge lanes and sibling streams.
    auto& list = context.list;
    list.enclosure.value.limits.max_concurrent_streams.value = kReceiverCount;
    list.receivers.value.count = kReceiverCount;
    context.execution.receiver_count = kReceiverCount;
    for (std::uint8_t i = 0; i < kReceiverCount; ++i) {
        auto& receiver = list.receivers.value.items[i];
        receiver = list.receivers.value.items[i % 2U];
        receiver.id = i;
        receiver.systems.mask = i % 4U < 2U ? 2U : 1U;
        receiver.binding.value = wire::Binding{static_cast<std::uint16_t>(i / 4U),
                                                static_cast<std::uint16_t>(i % 4U)};
        receiver.capabilities.controls = 0;
        receiver.capabilities.lnb.values.count = 0;
        if (i % 4U < 2U) {
            receiver.capabilities.controls = wire::kControlBitLnb;
            receiver.capabilities.lnb.values.count = 2;
            receiver.capabilities.lnb.values.items[0] = 0;
            receiver.capabilities.lnb.values.items[1] = 15;
        }
        context.receiver_index[i] = i;
        context.execution.receiver_lane[i] = i < 4U ?
            ControlWorkerLane::tuner_dev1 : ControlWorkerLane::tuner_dev2;
    }
    return wire::validate_list_response(list) == Error::OK;
}

Result<std::unique_ptr<PosixControlServer>> create_typed_server(
    const EndpointConfig& endpoint, CardService& card_service,
    TunerService& tuner_service, const char* instance,
    TunerStreamControl* stream_control = nullptr)
{
    static std::atomic<std::uint8_t> next_epoch_seed{1U};
    ControlContext context{};
    if (!make_control_context(tuner_service, instance,
                              next_epoch_seed.fetch_add(1U), context))
        return Result<std::unique_ptr<PosixControlServer>>::failure(Error::INTERNAL);
    return PosixControlServer::create(endpoint, card_service, tuner_service,
                                      context, stream_control);
}

class ControlStream final : public TunerStreamControl {
public:
    Result<void> attach(const TunerAttachment& value) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t index = 0U; index < active.size(); ++index) {
            if (active[index] && active_identities[index].attachment_id ==
                    value.attachment_id) {
                return Result<void>::failure(Error::BUSY);
            }
        }
        if (value.receiver >= active.size())
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        active[value.receiver] = true;
        active_identities[value.receiver] = value;
        identity = value;
        attached = true;
        final_available_by_receiver[value.receiver] = false;
        final_terminal = TunerStreamTerminal::stopped;
        return Result<void>::success();
    }

    Result<void> detach(const TunerAttachment& value) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t receiver = active.size();
        for (std::size_t index = 0U; index < active.size(); ++index) {
            if (active[index] &&
                active_identities[index].attachment_id == value.attachment_id) {
                receiver = index;
                break;
            }
        }
        if (receiver == active.size())
            return Result<void>::failure(Error::NOT_FOUND);
        active[receiver] = false;
        attached = false;
        for (const bool value_active : active) attached = attached || value_active;
        ++detach_calls;
        final_identity = value;
        final_available = true;
        final_identities[receiver] = value;
        final_available_by_receiver[receiver] = true;
        final_terminals[receiver] = final_terminal;
        changed.notify_all();
        return Result<void>::success();
    }

    Result<TunerStreamCounters> stats(
        const TunerAttachment& value) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t index = 0U; index < active.size(); ++index) {
            if (active[index] && active_identities[index].attachment_id ==
                    value.attachment_id)
                return Result<TunerStreamCounters>::success(counters);
        }
        return Result<TunerStreamCounters>::failure(Error::NOT_FOUND);
    }

    Result<TunerStreamFinalSnapshot> final_snapshot(
        const TunerAttachment& value) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t index = 0U; index < final_available_by_receiver.size();
             ++index) {
            if (final_available_by_receiver[index] &&
                final_identities[index].attachment_id == value.attachment_id)
                return Result<TunerStreamFinalSnapshot>::success(
                    TunerStreamFinalSnapshot{
                        counters, static_cast<std::uint8_t>(final_terminals[index])});
        }
        return Result<TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND);
    }

    Result<TunerStreamReadResult> read(
        const TunerAttachment& value, MutableByteView output, Timeout) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        bool active_match = false;
        for (std::size_t index = 0U; index < active.size(); ++index) {
            if (active[index] && active_identities[index].attachment_id ==
                    value.attachment_id) {
                active_match = true;
                break;
            }
        }
        if (active_match && (emit_data || continuous_data)) {
            if (output.size < data_packet.size())
                return Result<TunerStreamReadResult>::failure(Error::BUFFER_TOO_SMALL);
            std::memcpy(output.data, data_packet.data(), data_packet.size());
            if (!continuous_data) emit_data = false;
            return Result<TunerStreamReadResult>::success(
                TunerStreamReadResult{data_packet.size(), false, false,
                                      TunerStreamTerminal::none});
        }
        if (active_match)
            return Result<TunerStreamReadResult>::success(
                TunerStreamReadResult{0U, false, true, TunerStreamTerminal::none});
        for (std::size_t index = 0U; index < final_available_by_receiver.size();
             ++index) {
            if (final_available_by_receiver[index] &&
                final_identities[index].attachment_id == value.attachment_id)
                return Result<TunerStreamReadResult>::success(
                    TunerStreamReadResult{0U, true, false, final_terminals[index]});
        }
        return Result<TunerStreamReadResult>::failure(Error::NOT_FOUND);
    }

    bool wait_for_detaches(std::size_t expected) noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return detach_calls >= expected;
        });
    }

    TunerStreamCounters counters{17U, 3196U, 1U, 2U, 3U, 4U, 5U, 6U};
    mutable std::mutex mutex;
    TunerAttachment identity{};
    TunerAttachment final_identity{};
    bool attached = false;
    bool final_available = false;
    bool emit_data = false;
    bool continuous_data = false;
    TunerStreamTerminal final_terminal = TunerStreamTerminal::stopped;
    std::size_t detach_calls = 0U;
    std::condition_variable changed;
    std::array<std::uint8_t, 188U> data_packet{};
    std::array<bool, kReceiverCount> active{};
    std::array<TunerAttachment, kReceiverCount> active_identities{};
    std::array<bool, kReceiverCount> final_available_by_receiver{};
    std::array<TunerAttachment, kReceiverCount> final_identities{};
    std::array<TunerStreamTerminal, kReceiverCount> final_terminals{};
};

class TunerNonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, kNonceLength> value{};
        value[0] = next++;
        return Result<std::array<std::uint8_t, kNonceLength>>::success(value);
    }
private:
    std::uint8_t next = 1U;
};

class TunerTime final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t milliseconds) noexcept override { now += milliseconds; }
private:
    std::uint64_t now = 0U;
};

class BlockedOperationGuard final {
public:
    BlockedOperationGuard(std::atomic<bool>& blocked,
                          std::condition_variable& condition,
                          std::thread& request,
                          std::atomic<bool>& stop, std::thread& server_thread,
                          PosixControlServer& server) noexcept
        : blocked_(blocked), condition_(condition), request_(request), stop_(stop),
          server_thread_(server_thread), server_(server)
    {
    }

    ~BlockedOperationGuard() noexcept
    {
        if (!active_) return;
        unblock_and_join();
        stop_.store(true);
        if (server_thread_.joinable()) server_thread_.join();
        (void)server_.shutdown();
    }

    void unblock_and_join() noexcept
    {
        blocked_.store(false);
        condition_.notify_all();
        if (request_.joinable()) request_.join();
    }

    void dismiss() noexcept { active_ = false; }

private:
    std::atomic<bool>& blocked_;
    std::condition_variable& condition_;
    std::thread& request_;
    std::atomic<bool>& stop_;
    std::thread& server_thread_;
    PosixControlServer& server_;
    bool active_ = true;
};

class ServerThreadGuard final {
public:
    ServerThreadGuard(std::atomic<bool>& stop, std::thread& thread,
                      PosixControlServer& server) noexcept
        : stop_(stop), thread_(thread), server_(server)
    {
    }

    ~ServerThreadGuard() noexcept
    {
        if (!active_) return;
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        (void)server_.shutdown();
    }

    void dismiss() noexcept { active_ = false; }

private:
    std::atomic<bool>& stop_;
    std::thread& thread_;
    PosixControlServer& server_;
    bool active_ = true;
};

class AttachCompletionBarrier final {
public:
    static void before_completion(void* context) noexcept
    {
        auto& barrier = *static_cast<AttachCompletionBarrier*>(context);
        std::unique_lock<std::mutex> lock(barrier.mutex);
        barrier.entered = true;
        barrier.changed.notify_all();
        barrier.changed.wait(lock, [&]() noexcept { return barrier.released; });
    }

    bool wait_until_entered() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return entered;
        });
    }

    void release() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

class AttachCompletionReleaseGuard final {
public:
    explicit AttachCompletionReleaseGuard(AttachCompletionBarrier& barrier) noexcept
        : barrier_(barrier)
    {
    }

    ~AttachCompletionReleaseGuard() noexcept
    {
        if (active_) barrier_.release();
    }

    void dismiss() noexcept { active_ = false; }

private:
    AttachCompletionBarrier& barrier_;
    bool active_ = true;
};

class StreamServerObservation final {
public:
    static void count_changed(void* context, std::size_t live,
                              std::size_t retired) noexcept
    {
        auto& observation = *static_cast<StreamServerObservation*>(context);
        std::lock_guard<std::mutex> lock(observation.mutex);
        observation.live = live;
        observation.retired = retired;
        if (retired != 0U) observation.saw_retired = true;
        observation.changed.notify_all();
    }

    static void partial_frame(void* context, std::uint64_t) noexcept
    {
        auto& observation = *static_cast<StreamServerObservation*>(context);
        std::lock_guard<std::mutex> lock(observation.mutex);
        ++observation.partial_frames;
        observation.changed.notify_all();
    }

    static void write_blocked(void* context, std::uint64_t) noexcept
    {
        auto& observation = *static_cast<StreamServerObservation*>(context);
        std::lock_guard<std::mutex> lock(observation.mutex);
        ++observation.write_blocks;
        observation.changed.notify_all();
    }

    bool wait_for_capacity(std::size_t expected_live,
                           std::size_t expected_retired) noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return live == expected_live && retired == expected_retired;
        });
    }

    bool wait_for_retired_owner(std::size_t expected_live) noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return live == expected_live && saw_retired;
        });
    }

    bool wait_for_partial() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return partial_frames != 0U;
        });
    }

    bool wait_for_write_blocked() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return write_blocks != 0U;
        });
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::size_t live = 0U;
    std::size_t retired = 0U;
    bool saw_retired = false;
    std::size_t partial_frames = 0U;
    std::size_t write_blocks = 0U;
};

class StreamServerHookGuard final {
public:
    explicit StreamServerHookGuard(StreamServerObservation& observation) noexcept
    {
        hooks_.stream_count_changed = &StreamServerObservation::count_changed;
        hooks_.stream_partial_frame = &StreamServerObservation::partial_frame;
        hooks_.stream_write_blocked = &StreamServerObservation::write_blocked;
        hooks_.context = &observation;
        set_control_server_test_hooks(&hooks_);
    }

    ~StreamServerHookGuard() noexcept
    {
        set_control_server_test_hooks(nullptr);
    }

    StreamServerHookGuard(const StreamServerHookGuard&) = delete;
    StreamServerHookGuard& operator=(const StreamServerHookGuard&) = delete;

private:
    ControlServerTestHooks hooks_{};
};

class FrameCollector final : public wire::FrameHandler {
public:
    FrameCollector() : payloads(64U) {}

    Result<void> on_frame(const wire::Frame& frame) noexcept override
    {
        if (count >= headers.size() || frame.payload.size > payloads[0U].size()) {
            return Result<void>::failure(Error::BUFFER_TOO_SMALL);
        }
        headers[count] = frame.header;
        if (frame.payload.size != 0U) {
            std::memcpy(payloads[count].data(), frame.payload.data, frame.payload.size);
        }
        payload_sizes[count] = frame.payload.size;
        ++count;
        return Result<void>::success();
    }

    void clear() noexcept
    {
        count = 0U;
        payload_sizes.fill(0U);
    }

    // A single nonblocking read can legally contain many small stream frames.
    // Keep enough bounded storage for one maximum transport read.  The payload
    // backing is on the heap because collectors also live on worker threads.
    std::array<wire::Header, 64U> headers{};
    std::vector<std::array<std::uint8_t, 1024U>> payloads;
    std::array<std::size_t, 64U> payload_sizes{};
    std::size_t count = 0U;
};

bool send_socket_bytes(SocketStream& stream, const std::uint8_t* data,
                       std::size_t size) noexcept;

Result<void> send_raw_request(SocketStream& stream, MessageType type,
                              std::uint32_t request_id, ByteView payload) noexcept
{
    std::vector<std::uint8_t> frame(kFrameHeaderSize + kMaxControlPayload, 0U);
    wire::Header header;
    header.opcode = static_cast<std::uint16_t>(type);
    header.request_id = request_id;
    header.payload_length = static_cast<std::uint32_t>(payload.size);
    const auto encoded = wire::encode_frame(header, payload,
        MutableByteView{frame.data(), frame.size()});
    if (!encoded) return Result<void>::failure(encoded.error());
    return send_socket_bytes(stream, frame.data(), encoded.value()) ?
        Result<void>::success() : Result<void>::failure(Error::DISCONNECTED);
}

bool send_socket_bytes(SocketStream& stream, const std::uint8_t* data,
                       std::size_t size) noexcept
{
    std::size_t offset = 0U;
    while (offset < size) {
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const ssize_t sent = ::send(stream.native_handle(), data + offset,
                                    size - offset, flags);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{stream.native_handle(), POLLOUT, 0};
            if (::poll(&descriptor, 1U, 2000) > 0) continue;
        }
        return false;
    }
    return true;
}

bool read_raw_frames(SocketStream& stream, wire::FrameReceiver& framer,
                     FrameCollector& collector, std::size_t count) noexcept
{
    std::array<std::uint8_t, 8192U> input{};
    for (std::size_t attempt = 0U; attempt < 200U && collector.count < count; ++attempt) {
        const auto read = stream.read_some(
            MutableByteView{input.data(), input.size()}, Timeout{20U});
        if (!read) {
            if (read.error() == Error::TIMEOUT) continue;
            return false;
        }
        if (!framer.feed(ByteView{input.data(), read.value()}, collector)) return false;
    }
    return collector.count >= count;
}

bool read_until_stream_end(SocketStream& stream, wire::FrameReceiver& framer,
                           wire::StreamEndEvent& end) noexcept
{
    FrameCollector frames;
    for (std::size_t attempt = 0U; attempt < 500U; ++attempt) {
        frames.clear();
        if (!read_raw_frames(stream, framer, frames, 1U)) return false;
        for (std::size_t index = 0U; index < frames.count; ++index) {
            if (frames.headers[index].opcode != wire::kOpcodeStreamEndEvent ||
                frames.headers[index].kind != wire::Kind::event)
                continue;
            const auto decoded = wire::decode_stream_end_event(
                ByteView{frames.payloads[index].data(), frames.payload_sizes[index]});
            if (!decoded) return false;
            end = decoded.value();
            return true;
        }
    }
    return false;
}

bool counters_are(const wire::StreamCounters& value, std::uint64_t packets,
                  std::uint64_t dropped)
{
    return value.entries.count == 2 &&
        value.entries.items[0].id == wire::kCounterIdTransmittedPackets &&
        value.entries.items[1].id == wire::kCounterIdQueueDroppedPackets &&
        value.entries.items[0].counter.measurement.state == wire::MeasureState::valid &&
        value.entries.items[1].counter.measurement.state == wire::MeasureState::valid &&
        value.entries.items[0].counter.measurement.value == packets &&
        value.entries.items[1].counter.measurement.value == dropped;
}

bool end_error_is(const wire::StreamEndEvent& end, ErrorCode code)
{
    return code == ErrorCode::OK ? !end.final.error.present :
        end.final.error.present && end.final.error.value.code == static_cast<std::uint32_t>(code);
}

bool raw_hello(SocketStream& stream, wire::FrameReceiver& framer,
               wire::Uuid& epoch) noexcept
{
    std::array<std::uint8_t, 24U> payload{};
    const auto payload_size = wire::encode_hello_request(
        wire::HelloRequest{{1, 0}, {1, 0}, 0, wire::kFeatureStateEvent},
        MutableByteView{payload.data(), payload.size()});
    if (!payload_size || !send_raw_request(
            stream, MessageType::HELLO, 1U,
            ByteView{payload.data(), payload_size.value()})) {
        return false;
    }
    FrameCollector frames;
    if (!read_raw_frames(stream, framer, frames, 1U) ||
        frames.headers[0U].opcode != wire::kOpcodeHello ||
        frames.headers[0U].kind != wire::Kind::response) return false;
    const auto hello = wire::decode_hello_response(
        ByteView{frames.payloads[0].data(), frames.payload_sizes[0]});
    if (!hello) return false;
    epoch = hello.value().epoch;
    return true;
}

class Session final : public CardProtocolSession {
public:
    Session()
    {
        atr_.bytes[0] = 0x3bU;
        atr_.bytes[1] = 0x00U;
        atr_.length = 2U;
    }
    Result<void> initialize() noexcept override
    {
        initialized_ = true;
        ++initialize_calls;
        return Result<void>::success();
    }
    Result<std::size_t> transmit(ByteView apdu, MutableByteView response) noexcept override
    {
        ++transmit_calls;
        if (!initialized_ || apdu.size == 0U || response.size < 2U) {
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
        }
        response.data[0] = 0x61U;
        response.data[1] = 0x00U;
        return Result<std::size_t>::success(2U);
    }
    bool initialized() const noexcept override { return initialized_; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override { initialized_ = false; }
    std::size_t initialize_calls = 0U;
    std::size_t transmit_calls = 0U;
private:
    CardAtr atr_{};
    bool initialized_ = false;
};

class EventSink final : public ControlEventSink {
public:
    void on_device_event(const DeviceEventPayload& event) noexcept override
    {
        last = event;
        ++count;
    }
    DeviceEventPayload last{};
    std::size_t count = 0U;
};

// Private typed test entrance. The small legacy-named input aggregates below
// describe case intentions only; no legacy codec/client is used on the socket.
class TypedControl final {
public:
    static Result<std::unique_ptr<TypedControl>> connect(
        const EndpointConfig& endpoint, std::uint32_t capabilities, Timeout)
    {
        auto wire_client = WireClient::connect(endpoint);
        if (!wire_client || !wire_client->hello(
                capabilities & kCapabilityCard ? wire::kFeatureCardService : 0,
                wire::kFeatureStateEvent))
            return Result<std::unique_ptr<TypedControl>>::failure(Error::PROTOCOL_ERROR);
        return Result<std::unique_ptr<TypedControl>>::success(
            std::unique_ptr<TypedControl>(new TypedControl(std::move(wire_client))));
    }
    Result<WireReply> exchange(std::uint16_t opcode, const std::vector<std::uint8_t>& bytes)
    {
        WireReply reply;
        if (!client->request(opcode, bytes, reply))
            return Result<WireReply>::failure(Error::DISCONNECTED);
        if (reply.header.kind == wire::Kind::error_response) {
            const auto error = wire::decode_error_response(reply.view());
            return Result<WireReply>::failure(error ?
                static_cast<Error>(error.value().error.code) : Error::PROTOCOL_ERROR);
        }
        return Result<WireReply>::success(std::move(reply));
    }
    template<typename Record>
    Result<WireReply> record(std::uint16_t opcode, const Record& value,
        Result<std::size_t> (*encoder)(const Record&, MutableByteView) noexcept)
    { return exchange(opcode, WireClient::encode(value, encoder)); }
    wire::AttachRequest attachment(std::uint64_t lease) const
    {
        const auto& started = starts.at(lease);
        return {client->epoch, lease, started.stream_id, started.attach_token};
    }
    void close() { client->close(); }
    std::unique_ptr<WireClient> client;
    std::map<std::uint64_t, std::uint64_t> revisions;
    std::map<std::uint64_t, wire::StartResponse> starts;
    std::map<std::uint64_t, wire::CardSessionResult> cards;
private:
    explicit TypedControl(std::unique_ptr<WireClient> value) : client(std::move(value)) {}
};

Result<WireReply> request(TypedControl& client, MessageType,
                         const AcquireRequestPayload& payload)
{
    return client.record(wire::kOpcodeAcquire,
        wire::AcquireRequest{client.client->epoch, payload.receiver_id},
        &wire::encode_acquire_request);
}
Result<WireReply> request(TypedControl& client, MessageType,
                         const TuneRequestPayload& payload)
{
    wire::TuneRequest tune;
    tune.epoch = client.client->epoch;
    tune.lease_id = payload.lease_id;
    tune.system = payload.system == System::ISDB_T ? wire::System::isdb_t : wire::System::isdb_s;
    tune.frequency_hz = payload.frequency_khz * 1000U;
    tune.timeout_ms = payload.timeout_ms;
    if (payload.slot != 0xffffU) {
        tune.selection = wire::Selection::slot;
        tune.selection_value = payload.slot;
    } else if (payload.stream_id != 0xffffU) {
        tune.selection = wire::Selection::tsid;
        tune.selection_value = payload.stream_id;
    }
    if (tune.system == wire::System::isdb_s) {
        tune.lnb_voltage.present = true;
        tune.lnb_voltage.value = payload.lnb_voltage;
    }
    auto reply = client.record(wire::kOpcodeTune, tune, &wire::encode_tune_request);
    if (reply) {
        auto result = wire::decode_tune_result(reply.value().view());
        if (!result) return Result<WireReply>::failure(result.error());
        client.revisions[payload.lease_id] = result.value().tune_revision;
    }
    return reply;
}
Result<WireReply> request(TypedControl& client, MessageType type,
                         const LeaseRequestPayload& payload)
{
    if (type == MessageType::RELEASE) {
        auto reply = client.record(wire::kOpcodeRelease,
            wire::ReleaseRequest{client.client->epoch, payload.lease_id},
            &wire::encode_release_request);
        if (reply) {
            const auto released = wire::decode_release_response(reply.value().view());
            if (!released) return Result<WireReply>::failure(released.error());
            if (released.value().error.present)
                return Result<WireReply>::failure(static_cast<Error>(released.value().error.value.code));
        }
        return reply;
    }
    if (type == MessageType::START_STREAM) {
        auto reply = client.record(wire::kOpcodeStartStream,
            wire::StartRequest{client.client->epoch, payload.lease_id,
                               client.revisions[payload.lease_id]}, &wire::encode_start_request);
        if (reply) {
            auto started = wire::decode_start_response(reply.value().view());
            if (!started) return Result<WireReply>::failure(started.error());
            client.starts[payload.lease_id] = started.value();
        }
        return reply;
    }
    return client.record(wire::kOpcodeStopStream,
        wire::StopRequest{client.client->epoch, payload.lease_id,
            client.starts[payload.lease_id].stream_id}, &wire::encode_stop_request);
}
Result<WireReply> request(TypedControl& client, MessageType,
                         const CardConnectRequestPayload&)
{
    auto reply = client.record(wire::kOpcodeCardConnect,
        wire::CardConnectRequest{{client.client->epoch, 0}, wire::Share::shared, 1},
        &wire::encode_card_connect_request);
    if (reply) {
        const auto card = wire::decode_card_session_result(reply.value().view());
        if (!card) return Result<WireReply>::failure(card.error());
        client.cards[card.value().card_handle] = card.value();
    }
    return reply;
}
Result<WireReply> request(TypedControl& client, MessageType,
                         const CardDispositionRequestPayload& payload)
{
    const auto& card = client.cards.at(payload.card_handle);
    return client.record(wire::kOpcodeCardDisconnect,
        wire::CardDisconnectRequest{{client.client->epoch, card.reader_id,
            card.bound_generation, card.card_handle}, wire::Disposition::leave},
        &wire::encode_card_disconnect_request);
}
Result<WireReply> empty_request(TypedControl& client, MessageType type)
{
    if (type == MessageType::LIST)
        return client.record(wire::kOpcodeList, wire::ListRequest{client.client->epoch},
                             &wire::encode_list_request);
    return client.record(wire::kOpcodeStatus,
        wire::StatusRequest{client.client->epoch, {wire::ScopeKind::enclosure, 0}},
        &wire::encode_status_request);
}

bool test_control_server_card_flow_and_shutdown()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* instance = "typed-card-flow";
    const EndpointConfig endpoint{runtime.path().c_str(), instance, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend backend;
    Session session;
    CardService service(backend, session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time);
    auto server_result = create_typed_server(endpoint, service, tuner_service, instance);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    const std::string socket_path = server->endpoint_path();
    const std::string stream_socket_path = server->stream_endpoint_path();
    const auto instance_path = std::filesystem::path(socket_path).parent_path();
    const auto product_path = instance_path.parent_path();
    CHECK(std::filesystem::is_directory(instance_path));
    CHECK(std::filesystem::is_directory(product_path));

    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{20U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto first = WireClient::connect(endpoint);
    auto second = WireClient::connect(endpoint);
    CHECK(first && second);
    CHECK(first->hello(wire::kFeatureCardService, wire::kFeatureStateEvent));
    CHECK(second->hello(wire::kFeatureCardService, 0U));
    CHECK((first->features & wire::kFeatureCardService) != 0U);
    CHECK((first->features & wire::kFeatureStateEvent) == 0U);

    wire::ListRequest list_request;
    list_request.epoch = first->epoch;
    WireReply listed;
    CHECK(first->request(wire::kOpcodeList,
        WireClient::encode(list_request, &wire::encode_list_request), listed));
    const auto list = wire::decode_list_response(listed.view());
    CHECK(list && list.value().receivers.present &&
          list.value().receivers.value.count == kReceiverCount &&
          list.value().readers.present && list.value().readers.value.count == 1U);
    CHECK(list.value().receivers.value.items[0].systems.mask == 0x02U);
    CHECK(list.value().receivers.value.items[1].systems.mask == 0x02U);
    CHECK(list.value().receivers.value.items[2].systems.mask == 0x01U);

    wire::ReaderKey reader{first->epoch, 0U};
    wire::CardStatusRequest initial_request{reader};
    WireReply initial_reply;
    CHECK(first->request(wire::kOpcodeCardStatus,
        WireClient::encode(initial_request, &wire::encode_card_status_request),
        initial_reply));
    const auto initial = wire::decode_card_status_response(initial_reply.view());
    CHECK(initial && initial.value().present && !initial.value().initialized &&
          initial.value().current_generation != 0U);

    wire::CardConnectRequest connect1;
    connect1.key = reader;
    connect1.share = wire::Share::shared;
    connect1.preferred_protocols = 1U;
    wire::CardConnectRequest connect2 = connect1;
    connect2.key.epoch = second->epoch;
    WireReply opened1;
    WireReply opened2;
    CHECK(first->request(wire::kOpcodeCardConnect,
        WireClient::encode(connect1, &wire::encode_card_connect_request), opened1));
    CHECK(second->request(wire::kOpcodeCardConnect,
        WireClient::encode(connect2, &wire::encode_card_connect_request), opened2));
    auto card1 = wire::decode_card_session_result(opened1.view());
    auto card2 = wire::decode_card_session_result(opened2.view());
    CHECK(card1 && card2 && card1.value().card_handle != card2.value().card_handle &&
          card1.value().bound_generation == initial.value().current_generation &&
          card2.value().bound_generation == initial.value().current_generation);
    CHECK(card1.value().atr.present && card1.value().atr.value.length == 2U &&
          card1.value().atr.value.bytes[0] == 0x3bU &&
          card1.value().atr.value.bytes[1] == 0x00U);

    wire::CardHandleKey key2{second->epoch, 0U, card2.value().bound_generation,
                             card2.value().card_handle};
    wire::CardReconnectRequest reconnect{key2, wire::Share::shared,
                                         wire::Disposition::leave, 1U};
    WireReply reconnect_reply;
    CHECK(second->request(wire::kOpcodeCardReconnect,
        WireClient::encode(reconnect, &wire::encode_card_reconnect_request),
        reconnect_reply));
    card2 = wire::decode_card_session_result(reconnect_reply.view());
    CHECK(card2 && card2.value().atr.present && card2.value().atr.value.length == 2U);
    key2.bound_generation = card2.value().bound_generation;

    wire::CardHandleKey key1{first->epoch, 0U, card1.value().bound_generation,
                             card1.value().card_handle};
    wire::CardResetRequest reset{key1};
    WireReply reset_reply;
    CHECK(first->request(wire::kOpcodeCardReset,
        WireClient::encode(reset, &wire::encode_card_reset_request), reset_reply));
    card1 = wire::decode_card_session_result(reset_reply.view());
    CHECK(card1 && card1.value().atr.present && card1.value().atr.value.length == 2U);
    key1.bound_generation = card1.value().bound_generation;

    wire::BeginTransactionRequest begin{key1};
    WireReply begin_reply;
    CHECK(first->request(wire::kOpcodeBeginTransaction,
        WireClient::encode(begin, &wire::encode_begin_transaction_request), begin_reply));
    CHECK(begin_reply.header.kind == wire::Kind::response);
    std::array<std::uint8_t, 2U> apdu{0x00U, 0x84U};
    wire::CardTransmitRequest transmit2;
    transmit2.key = key2;
    transmit2.protocol = wire::CardProtocol::t1;
    transmit2.command.length = apdu.size();
    std::copy(apdu.begin(), apdu.end(), transmit2.command.bytes.begin());
    WireReply busy;
    CHECK(second->request(wire::kOpcodeCardTransmit,
        WireClient::encode(transmit2, &wire::encode_card_transmit_request), busy));
    const auto busy_error = wire::decode_error_response(busy.view());
    CHECK(busy.header.kind == wire::Kind::error_response && busy_error &&
          busy_error.value().error.code == static_cast<std::uint32_t>(Error::BUSY));
    wire::EndTransactionRequest end{key1, wire::Disposition::leave};
    WireReply end_reply;
    CHECK(first->request(wire::kOpcodeEndTransaction,
        WireClient::encode(end, &wire::encode_end_transaction_request), end_reply));
    CHECK(end_reply.header.kind == wire::Kind::response);

    wire::CardTransmitRequest transmit1;
    transmit1.key = key1;
    transmit1.protocol = wire::CardProtocol::t1;
    transmit1.command.length = apdu.size();
    std::copy(apdu.begin(), apdu.end(), transmit1.command.bytes.begin());
    WireReply transmitted;
    CHECK(first->request(wire::kOpcodeCardTransmit,
        WireClient::encode(transmit1, &wire::encode_card_transmit_request), transmitted));
    const auto response = wire::decode_card_transmit_response(transmitted.view());
    CHECK(response && response.value().response.length == 2U &&
          response.value().response.bytes[0] == 0x61U &&
          response.value().response.bytes[1] == 0x00U);

    wire::CardDisconnectRequest disconnect2{key2, wire::Disposition::leave};
    WireReply disconnect_reply;
    CHECK(second->request(wire::kOpcodeCardDisconnect,
        WireClient::encode(disconnect2, &wire::encode_card_disconnect_request),
        disconnect_reply));
    const auto disconnected = wire::decode_card_disconnect_response(
        disconnect_reply.view());
    CHECK(disconnected && disconnected.value().consumed &&
          disconnected.value().quiesced);
    CHECK(service.handle_count() == 1U && service.powered());

    backend.present.store(false);
    WireReply removed_reply;
    CHECK(first->request(wire::kOpcodeCardStatus,
        WireClient::encode(initial_request, &wire::encode_card_status_request),
        removed_reply));
    const auto removed = wire::decode_card_status_response(removed_reply.view());
    CHECK(removed && !removed.value().present &&
          removed.value().current_generation > key1.bound_generation);
    WireReply stale_reply;
    CHECK(first->request(wire::kOpcodeCardTransmit,
        WireClient::encode(transmit1, &wire::encode_card_transmit_request),
        stale_reply));
    const auto stale = wire::decode_error_response(stale_reply.view());
    CHECK(stale_reply.header.kind == wire::Kind::error_response && stale &&
          stale.value().error.code == static_cast<std::uint32_t>(Error::CARD_REMOVED));

    first->close();
    second->close();
    const auto cleanup_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(3);
    while (service.handle_count() != 0U &&
           std::chrono::steady_clock::now() < cleanup_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10U));
    stop.store(true);
    server_thread.join();
    server_guard.dismiss();
    CHECK(server_ok.load());
    CHECK(service.handle_count() == 0U && !service.powered());
    CHECK(server->shutdown());
    CHECK(!std::filesystem::exists(socket_path));
    CHECK(!std::filesystem::exists(stream_socket_path));
    CHECK(!std::filesystem::exists(instance_path));
    CHECK(!std::filesystem::exists(product_path));
    return true;
}

bool test_card_client_ownership_contract()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    const EndpointConfig endpoint{runtime.path().c_str(), "card-contract",
                                  kControlEndpointName, EndpointAccess::private_user};
    Backend backend;
    Session session;
    CardService service(backend, session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time);
    auto server = create_typed_server(endpoint, service, tuner_service, "card-contract");
    CHECK(server);
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread poller([&]() noexcept {
        while (!stop.load()) {
            if (!server.value()->poll_once(Timeout{20U})) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard guard(stop, poller, *server.value());
    auto first = WireClient::connect(endpoint);
    auto second = WireClient::connect(endpoint);
    auto third = WireClient::connect(endpoint);
    CHECK(first && second && third);
    CHECK(first->hello(wire::kFeatureCardService, 0U));
    CHECK(second->hello(wire::kFeatureCardService, 0U));
    CHECK(third->hello(wire::kFeatureCardService, 0U));
    const auto open = [](WireClient& client, wire::Share share,
                         wire::CardSessionResult& result) {
        wire::CardConnectRequest value;
        value.key = wire::ReaderKey{client.epoch, 0U};
        value.share = share;
        value.preferred_protocols = 1U;
        WireReply reply;
        if (!client.request(wire::kOpcodeCardConnect,
                            WireClient::encode(value, &wire::encode_card_connect_request),
                            reply) || reply.header.kind != wire::Kind::response)
            return false;
        const auto decoded = wire::decode_card_session_result(reply.view());
        if (!decoded) return false;
        result = decoded.value();
        return true;
    };
    const auto key = [](const WireClient& client, const wire::CardSessionResult& card) {
        return wire::CardHandleKey{client.epoch, 0U, card.bound_generation,
                                   card.card_handle};
    };
    const auto error_is = [](const WireReply& reply, Error expected) {
        const auto decoded = wire::decode_error_response(reply.view());
        return reply.header.kind == wire::Kind::error_response && decoded &&
               decoded.value().error.code == static_cast<std::uint32_t>(expected);
    };
    const auto send = [](WireClient& client, const wire::CardHandleKey& handle,
                         WireReply& reply) {
        wire::CardTransmitRequest value;
        value.key = handle;
        value.protocol = wire::CardProtocol::t1;
        value.command.length = 2U;
        value.command.bytes[0] = 0x00U;
        value.command.bytes[1] = 0x84U;
        return client.request(wire::kOpcodeCardTransmit,
                              WireClient::encode(value, &wire::encode_card_transmit_request),
                              reply);
    };
    wire::CardSessionResult card1;
    wire::CardSessionResult card2;
    CHECK(open(*first, wire::Share::shared, card1));
    CHECK(open(*second, wire::Share::shared, card2));
    CHECK(card1.card_handle != 0U && card2.card_handle != 0U &&
          card1.card_handle != card2.card_handle);
    WireReply reply;
    wire::CardConnectRequest exclusive;
    exclusive.key = wire::ReaderKey{third->epoch, 0U};
    exclusive.share = wire::Share::exclusive;
    exclusive.preferred_protocols = 1U;
    CHECK(third->request(wire::kOpcodeCardConnect,
                         WireClient::encode(exclusive, &wire::encode_card_connect_request),
                         reply));
    CHECK(error_is(reply, Error::BUSY));

    wire::CardResetRequest foreign{key(*first, card1)};
    CHECK(second->request(wire::kOpcodeCardReset,
                          WireClient::encode(foreign, &wire::encode_card_reset_request),
                          reply));
    CHECK(error_is(reply, Error::NOT_FOUND));
    wire::CardReconnectRequest upgrade{key(*second, card2), wire::Share::exclusive,
                                        wire::Disposition::leave, 1U};
    CHECK(second->request(wire::kOpcodeCardReconnect,
                          WireClient::encode(upgrade, &wire::encode_card_reconnect_request),
                          reply));
    CHECK(error_is(reply, Error::BUSY));

    wire::BeginTransactionRequest begin1{key(*first, card1)};
    CHECK(first->request(wire::kOpcodeBeginTransaction,
                         WireClient::encode(begin1, &wire::encode_begin_transaction_request),
                         reply));
    wire::BeginTransactionRequest begin2{key(*second, card2)};
    CHECK(second->request(wire::kOpcodeBeginTransaction,
                          WireClient::encode(begin2, &wire::encode_begin_transaction_request),
                          reply));
    CHECK(error_is(reply, Error::BUSY));
    CHECK(send(*second, key(*second, card2), reply));
    CHECK(error_is(reply, Error::BUSY));
    wire::CardResetRequest reset1{key(*first, card1)};
    CHECK(first->request(wire::kOpcodeCardReset,
                         WireClient::encode(reset1, &wire::encode_card_reset_request), reply));
    CHECK(send(*second, key(*second, card2), reply));
    CHECK(error_is(reply, Error::BUSY));
    wire::EndTransactionRequest end1{key(*first, card1), wire::Disposition::reset};
    CHECK(first->request(wire::kOpcodeEndTransaction,
                         WireClient::encode(end1, &wire::encode_end_transaction_request),
                         reply));
    CHECK(reply.header.kind == wire::Kind::response);
    CHECK(send(*second, key(*second, card2), reply));
    const auto response = wire::decode_card_transmit_response(reply.view());
    CHECK(reply.header.kind == wire::Kind::response && response &&
          response.value().response.length == 2U &&
          response.value().response.bytes[0] == 0x61U &&
          response.value().response.bytes[1] == 0x00U);

    CHECK(first->request(wire::kOpcodeBeginTransaction,
                         WireClient::encode(begin1, &wire::encode_begin_transaction_request),
                         reply));
    first->close();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        CHECK(second->request(wire::kOpcodeBeginTransaction,
                              WireClient::encode(begin2,
                                  &wire::encode_begin_transaction_request), reply));
        if (reply.header.kind == wire::Kind::response) break;
        CHECK(error_is(reply, Error::BUSY) && std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10U));
    }
    wire::EndTransactionRequest end2{key(*second, card2), wire::Disposition::leave};
    CHECK(second->request(wire::kOpcodeEndTransaction,
                          WireClient::encode(end2, &wire::encode_end_transaction_request),
                          reply));
    upgrade = wire::CardReconnectRequest{key(*second, card2), wire::Share::exclusive,
                                         wire::Disposition::reset, 1U};
    CHECK(second->request(wire::kOpcodeCardReconnect,
                          WireClient::encode(upgrade, &wire::encode_card_reconnect_request),
                          reply));
    const auto upgraded = wire::decode_card_session_result(reply.view());
    CHECK(reply.header.kind == wire::Kind::response && upgraded &&
          upgraded.value().atr.present);
    exclusive.key = wire::ReaderKey{third->epoch, 0U};
    exclusive.share = wire::Share::shared;
    CHECK(third->request(wire::kOpcodeCardConnect,
                         WireClient::encode(exclusive, &wire::encode_card_connect_request),
                         reply));
    CHECK(error_is(reply, Error::BUSY));
    wire::CardDisconnectRequest disconnect2{key(*second, upgraded.value()),
                                             wire::Disposition::reset};
    CHECK(second->request(wire::kOpcodeCardDisconnect,
                          WireClient::encode(disconnect2,
                              &wire::encode_card_disconnect_request), reply));
    CHECK(wire::decode_card_disconnect_response(reply.view()));
    CHECK(send(*second, key(*second, upgraded.value()), reply));
    CHECK(error_is(reply, Error::NOT_FOUND));
    exclusive.share = wire::Share::exclusive;
    CHECK(third->request(wire::kOpcodeCardConnect,
                         WireClient::encode(exclusive, &wire::encode_card_connect_request),
                         reply));
    const auto final_card = wire::decode_card_session_result(reply.view());
    CHECK(final_card && final_card.value().card_handle != 0U);
    wire::CardDisconnectRequest disconnect3{key(*third, final_card.value()),
                                             wire::Disposition::leave};
    CHECK(third->request(wire::kOpcodeCardDisconnect,
                         WireClient::encode(disconnect3,
                             &wire::encode_card_disconnect_request), reply));

    auto strict = WireClient::connect(endpoint);
    auto observer = WireClient::connect(endpoint);
    CHECK(strict && observer);
    CHECK(strict->hello(wire::kFeatureCardService, 0U));
    CHECK(observer->hello(wire::kFeatureCardService, 0U));
    CHECK(strict->features == wire::kFeatureCardService &&
          observer->features == wire::kFeatureCardService);
    wire::CardSessionResult strict1;
    wire::CardSessionResult strict2;
    wire::CardSessionResult observed;
    CHECK(open(*strict, wire::Share::shared, strict1));
    CHECK(open(*strict, wire::Share::shared, strict2));
    CHECK(open(*observer, wire::Share::shared, observed));
    wire::CardResetRequest initialize{key(*strict, strict1)};
    CHECK(strict->request(wire::kOpcodeCardReset,
        WireClient::encode(initialize, &wire::encode_card_reset_request), reply));
    const auto initialized = wire::decode_card_session_result(reply.view());
    CHECK(reply.header.kind == wire::Kind::response && initialized);
    strict1 = initialized.value();
    wire::CardHandleStatusRequest handle_status{key(*strict, strict1)};
    CHECK(strict->request(wire::kOpcodeCardHandleStatus,
        WireClient::encode(handle_status, &wire::encode_card_handle_status_request), reply));
    auto snapshot = wire::decode_card_session_result(reply.view());
    CHECK(reply.header.kind == wire::Kind::response && snapshot &&
          snapshot.value().present && snapshot.value().initialized &&
          snapshot.value().atr.present && snapshot.value().atr.value.length > 0U);
    wire::CardHandleStatusRequest foreign_status{key(*observer, observed)};
    CHECK(strict->request(wire::kOpcodeCardHandleStatus,
        WireClient::encode(foreign_status, &wire::encode_card_handle_status_request), reply));
    CHECK(error_is(reply, Error::NOT_FOUND));
    const auto observe = [&](bool inserted) {
        backend.present.store(inserted);
        wire::CardStatusRequest status{wire::ReaderKey{observer->epoch, 0U}};
        if (!observer->request(wire::kOpcodeCardStatus,
            WireClient::encode(status, &wire::encode_card_status_request), reply) ||
            reply.header.kind != wire::Kind::response)
            return false;
        const auto decoded = wire::decode_card_status_response(reply.view());
        return decoded && decoded.value().present == inserted;
    };
    wire::BeginTransactionRequest generation_begin{key(*strict, strict1)};
    CHECK(strict->request(wire::kOpcodeBeginTransaction,
        WireClient::encode(generation_begin, &wire::encode_begin_transaction_request), reply));
    CHECK(observe(false) && observe(true));
    CHECK(send(*strict, key(*strict, strict1), reply));
    CHECK(error_is(reply, Error::CARD_REMOVED));
    handle_status = wire::CardHandleStatusRequest{key(*strict, strict1)};
    CHECK(strict->request(wire::kOpcodeCardHandleStatus,
        WireClient::encode(handle_status, &wire::encode_card_handle_status_request), reply));
    CHECK(error_is(reply, Error::CARD_REMOVED));
    generation_begin = wire::BeginTransactionRequest{key(*strict, strict2)};
    CHECK(strict->request(wire::kOpcodeBeginTransaction,
        WireClient::encode(generation_begin, &wire::encode_begin_transaction_request), reply));
    CHECK(error_is(reply, Error::CARD_REMOVED));
    CHECK(send(*observer, key(*observer, observed), reply));
    CHECK(error_is(reply, Error::CARD_REMOVED));
    wire::CardReconnectRequest reconnect_observer{key(*observer, observed),
        wire::Share::shared, wire::Disposition::leave, 1U};
    CHECK(observer->request(wire::kOpcodeCardReconnect,
        WireClient::encode(reconnect_observer, &wire::encode_card_reconnect_request), reply));
    const auto observed_again = wire::decode_card_session_result(reply.view());
    CHECK(reply.header.kind == wire::Kind::response && observed_again);
    observed = observed_again.value();
    CHECK(send(*observer, key(*observer, observed), reply));
    CHECK(reply.header.kind == wire::Kind::response);
    wire::CardReconnectRequest reconnect1{key(*strict, strict1), wire::Share::shared,
                                          wire::Disposition::leave, 1U};
    CHECK(strict->request(wire::kOpcodeCardReconnect,
        WireClient::encode(reconnect1, &wire::encode_card_reconnect_request), reply));
    strict1 = wire::decode_card_session_result(reply.view()).value();
    CHECK(send(*strict, key(*strict, strict1), reply));
    CHECK(reply.header.kind == wire::Kind::response);
    CHECK(send(*strict, key(*strict, strict2), reply));
    CHECK(error_is(reply, Error::CARD_REMOVED));
    wire::CardResetRequest reset2{key(*strict, strict2)};
    CHECK(strict->request(wire::kOpcodeCardReset,
        WireClient::encode(reset2, &wire::encode_card_reset_request), reply));
    strict2 = wire::decode_card_session_result(reply.view()).value();
    CHECK(send(*strict, key(*strict, strict2), reply));
    CHECK(reply.header.kind == wire::Kind::response);
    CHECK(observe(false) && observe(true));
    for (const auto& item : {std::pair<WireClient*, wire::CardSessionResult*>{strict.get(), &strict1},
                             {strict.get(), &strict2}, {observer.get(), &observed}}) {
        wire::CardDisconnectRequest disconnect{key(*item.first, *item.second),
                                                wire::Disposition::leave};
        CHECK(item.first->request(wire::kOpcodeCardDisconnect,
            WireClient::encode(disconnect, &wire::encode_card_disconnect_request), reply));
    }
    stop.store(true);
    poller.join();
    CHECK(server_ok.load());
    CHECK(server.value()->shutdown());
    guard.dismiss();
    CHECK(service.handle_count() == 0U && !service.powered());
    return true;
}

bool test_no_hello_timeout_and_malformed_disconnect()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000961";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend backend;
    Session session;
    CardService service(backend, session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    auto server_result = create_typed_server(endpoint, service, tuner_service, serial);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());

    std::atomic<bool> stop{false};
    std::thread server_thread([&]() {
        while (!stop.load()) (void)server->poll_once(Timeout{10U});
    });
    auto raw = SocketStream::connect(endpoint, Timeout{1000U});
    CHECK(raw);
    std::array<std::uint8_t, 8U> buffer{};
    const auto timeout = raw.value().read_some(
        MutableByteView{buffer.data(), buffer.size()}, Timeout{20U});
    CHECK(!timeout && timeout.error() == Error::TIMEOUT);

    const std::array<std::uint8_t, kFrameHeaderSize> malformed{};
    const ssize_t sent = ::send(raw.value().native_handle(), malformed.data(),
                                malformed.size(), 0);
    CHECK(sent == static_cast<ssize_t>(malformed.size()));
    Result<std::size_t> disconnected = Result<std::size_t>::failure(Error::TIMEOUT);
    for (std::size_t attempt = 0U; attempt < 100U; ++attempt) {
        disconnected = raw.value().read_some(
            MutableByteView{buffer.data(), buffer.size()}, Timeout{20U});
        if (!disconnected && disconnected.error() == Error::DISCONNECTED) break;
    }
    CHECK(!disconnected && disconnected.error() == Error::DISCONNECTED);
    stop.store(true);
    server_thread.join();
    CHECK(server->shutdown());
    return true;
}

bool test_control_server_tuner_flow()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000962";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    auto server_result = create_typed_server(endpoint, card_service, tuner_service, serial,
                                             &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());

    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{20U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });

    ServerThreadGuard server_guard(stop, server_thread, *server);
    auto first_result = TypedControl::connect(
        endpoint, kCapabilityEvents | kCapabilityStreamStats, Timeout{2000U});
    auto second_result = TypedControl::connect(
        endpoint, kCapabilityEvents | kCapabilityStreamStats, Timeout{2000U});
    CHECK(first_result && second_result);
    std::unique_ptr<TypedControl> first = std::move(first_result.value());
    std::unique_ptr<TypedControl> second = std::move(second_result.value());

    const auto terrestrial = request(*first, MessageType::ACQUIRE,
                                      AcquireRequestPayload{2U});
    CHECK(terrestrial);
    const auto terrestrial_lease = wire::decode_acquire_response(
        ByteView{terrestrial.value().payload.data(), terrestrial.value().payload.size()});
    CHECK(terrestrial_lease && terrestrial_lease.value().lease_id != 0U);
    const auto busy = request(*second, MessageType::ACQUIRE,
                               AcquireRequestPayload{2U});
    CHECK(!busy && busy.error() == Error::BUSY);
    const auto wrong_release = request(
        *second, MessageType::RELEASE,
        LeaseRequestPayload{terrestrial_lease.value().lease_id});
    CHECK(!wrong_release && wrong_release.error() == Error::NOT_FOUND);

    const auto satellite = request(*first, MessageType::ACQUIRE,
                                   AcquireRequestPayload{0U});
    CHECK(satellite);
    const auto satellite_lease = wire::decode_acquire_response(
        ByteView{satellite.value().payload.data(), satellite.value().payload.size()});
    CHECK(satellite_lease);

    const auto denied_lnb = request(
        *first, MessageType::TUNE,
        TuneRequestPayload{satellite_lease.value().lease_id, System::ISDB_S,
                           1200000U, 0xffffU, 1U, 0U, 15U, 100U});
    CHECK(!denied_lnb && denied_lnb.error() == Error::UNSUPPORTED);
    CHECK(tuner_backend.lnb_gpio_calls.load() == 0U);

    const auto terrestrial_tune = request(
        *first, MessageType::TUNE,
        TuneRequestPayload{terrestrial_lease.value().lease_id, System::ISDB_T,
                           500000U, 0xffffU, 0xffffU, 6000000U, 0U, 100U});
    CHECK(terrestrial_tune);
    const auto terrestrial_response = wire::decode_tune_result(
        ByteView{terrestrial_tune.value().payload.data(),
                 terrestrial_tune.value().payload.size()});
    CHECK(terrestrial_response &&
          terrestrial_response.value().lock.state == wire::MeasureState::valid &&
          terrestrial_response.value().lock.value);

    const auto satellite_tune = request(
        *first, MessageType::TUNE,
        TuneRequestPayload{satellite_lease.value().lease_id, System::ISDB_S,
                           1200000U, 0xffffU, 1U, 0U, 0U, 100U});
    CHECK(satellite_tune);
    CHECK(tuner_backend.last_system[2U] == System::ISDB_T &&
          tuner_backend.last_system[0U] == System::ISDB_S &&
          tuner_backend.last_frequency_hz[2U] == 500000000U &&
          tuner_backend.last_frequency_hz[0U] == 1200000000U);

    CHECK(request(*first, MessageType::START_STREAM,
                  LeaseRequestPayload{terrestrial_lease.value().lease_id}));
    const EndpointConfig data_endpoint{runtime.path().c_str(), serial,
        kStreamEndpointName, EndpointAccess::private_user};
    auto data_stream = SocketStream::connect(data_endpoint, Timeout{2000});
    CHECK(data_stream);
    std::array<std::uint8_t, wire::kMaxControlPayload> data_storage{};
    wire::FrameReceiver data_framer({data_storage.data(), data_storage.size()});
    auto attach_bytes = WireClient::encode(first->attachment(terrestrial_lease.value().lease_id),
                                           &wire::encode_attach_request);
    CHECK(send_raw_request(data_stream.value(), MessageType::ATTACH_STREAM, 1,
                           {attach_bytes.data(), attach_bytes.size()}));
    FrameCollector attached;
    CHECK(read_raw_frames(data_stream.value(), data_framer, attached, 1) &&
          attached.headers[0].kind == wire::Kind::response);
    const auto live_stats = empty_request(*first, MessageType::STATUS);
    CHECK(live_stats);
    const auto live_counters = wire::decode_status_response(
        ByteView{live_stats.value().payload.data(), live_stats.value().payload.size()});
    CHECK(live_counters && live_counters.value().receivers.present &&
          live_counters.value().receivers.value.items[2].stream_statistics.present &&
          counters_are(live_counters.value().receivers.value.items[2].stream_statistics.value,
                       17U, 4U));
    const auto stream_stats = first->record(wire::kOpcodeStatus,
        wire::StatusRequest{first->client->epoch,
            {wire::ScopeKind::stream, first->starts.at(terrestrial_lease.value().lease_id).stream_id}},
        &wire::encode_status_request);
    CHECK(stream_stats);
    const auto scoped_stats = wire::decode_status_response(stream_stats.value().view());
    CHECK(scoped_stats && scoped_stats.value().receivers.value.count == 1 &&
          scoped_stats.value().receivers.value.items[0].descriptor.id == 2 &&
          counters_are(scoped_stats.value().receivers.value.items[0].stream_statistics.value,
                       17U, 4U));
    const auto stopped_stream = request(
        *first, MessageType::STOP_STREAM,
        LeaseRequestPayload{terrestrial_lease.value().lease_id});
    CHECK(stopped_stream);
    const auto final_counters = wire::decode_stop_response(
        ByteView{stopped_stream.value().payload.data(),
                 stopped_stream.value().payload.size()});
    CHECK(final_counters && final_counters.value().quiesced &&
          final_counters.value().final.statistics.entries.items[0].counter.measurement.value == 17U &&
          final_counters.value().final.statistics.entries.items[1].counter.measurement.value == 4U);
    const auto native_final = stream_control.final_snapshot(stream_control.final_identity);
    CHECK(native_final && native_final.value().counters.bytes == 3196U &&
          native_final.value().counters.usb_errors == 5U);

    const auto status_response = empty_request(*first, MessageType::STATUS);
    CHECK(status_response);
    const auto status = wire::decode_status_response(
        ByteView{status_response.value().payload.data(), status_response.value().payload.size()});
    CHECK(status && status.value().receivers.present &&
          status.value().receivers.value.items[0].state == wire::ReceiverState::tuned &&
          status.value().receivers.value.items[2].state == wire::ReceiverState::tuned &&
          status.value().receivers.value.items[2].last_stream.present &&
          counters_are(status.value().receivers.value.items[2].last_stream.value.statistics,
                       17U, 4U));
    const auto retained_stats = first->record(wire::kOpcodeStatus,
        wire::StatusRequest{first->client->epoch,
            {wire::ScopeKind::stream, first->starts.at(terrestrial_lease.value().lease_id).stream_id}},
        &wire::encode_status_request);
    CHECK(retained_stats);
    const auto retained = wire::decode_status_response(retained_stats.value().view());
    CHECK(retained && !retained.value().receivers.value.items[0].stream.present &&
          counters_are(retained.value().receivers.value.items[0].last_stream.value.statistics,
                       17U, 4U));
    const auto revision_before_retune = status.value().receivers.value.items[2].tune_revision;
    const auto retuned = request(
        *first, MessageType::TUNE,
        TuneRequestPayload{terrestrial_lease.value().lease_id, System::ISDB_T,
                           500000U, 0xffffU, 0xffffU, 6000000U, 0U, 100U});
    CHECK(retuned);
    const auto retune_status_response = empty_request(*first, MessageType::STATUS);
    CHECK(retune_status_response);
    const auto retune_status = wire::decode_status_response(
        ByteView{retune_status_response.value().payload.data(),
                 retune_status_response.value().payload.size()});
    // UDTV makes accepted tune revisions explicit even for an identical retune.
    CHECK(retune_status && retune_status.value().receivers.value.items[2].tune_revision >
          revision_before_retune &&
          retune_status.value().receivers.value.items[2].state == wire::ReceiverState::tuned);

    auto raw_result = SocketStream::connect(endpoint, Timeout{2000U});
    CHECK(raw_result);
    SocketStream raw = std::move(raw_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> raw_storage{};
    wire::FrameReceiver raw_framer(MutableByteView{raw_storage.data(), raw_storage.size()});
    FrameCollector raw_frames;
    std::array<std::uint8_t, 24U> hello_payload{};
    const auto hello_size = wire::encode_hello_request(
        wire::HelloRequest{{1, 0}, {1, 0}, 0, wire::kFeatureStateEvent},
        MutableByteView{hello_payload.data(), hello_payload.size()});
    CHECK(hello_size && send_raw_request(
        raw, MessageType::HELLO, 1U,
        ByteView{hello_payload.data(), hello_size.value()}));
    CHECK(read_raw_frames(raw, raw_framer, raw_frames, 1U) &&
          raw_frames.headers[0U].opcode == wire::kOpcodeHello &&
          raw_frames.headers[0U].kind == wire::Kind::response);
    const auto raw_hello_response = wire::decode_hello_response(
        {raw_frames.payloads[0].data(), raw_frames.payload_sizes[0]});
    CHECK(raw_hello_response &&
          (raw_hello_response.value().features & wire::kFeatureStateEvent) == 0);
    raw_frames.clear();
    const auto acquire_payload = WireClient::encode(
        wire::AcquireRequest{raw_hello_response.value().epoch, 1}, &wire::encode_acquire_request);
    // UDTV ordinary requests require a nonzero ID. Send an invalid raw envelope
    // deliberately, bypassing the encoder's own validation, then reconnect.
    auto zero_frame = WireClient::frame(wire::kOpcodeAcquire, 2,
        {acquire_payload.data(), acquire_payload.size()});
    CHECK(zero_frame.size() >= wire::kHeaderSize);
    std::fill(zero_frame.begin() + 12, zero_frame.begin() + 16, 0);
    CHECK(send_socket_bytes(raw, zero_frame.data(), zero_frame.size()));
    std::array<std::uint8_t, 1> zero_read{};
    const auto zero_closed = raw.read_some({zero_read.data(), zero_read.size()}, Timeout{2000});
    CHECK(!zero_closed && zero_closed.error() == Error::DISCONNECTED);
    raw.close();
    raw_result = SocketStream::connect(endpoint, Timeout{2000});
    CHECK(raw_result);
    raw = std::move(raw_result.value());
    raw_framer.reset();
    wire::Uuid raw_epoch{};
    CHECK(raw_hello(raw, raw_framer, raw_epoch));
    CHECK(send_raw_request(raw, MessageType::ACQUIRE, 2U,
                           ByteView{acquire_payload.data(), acquire_payload.size()}));
    CHECK(read_raw_frames(raw, raw_framer, raw_frames, 1U) &&
          raw_frames.headers[0U].opcode == wire::kOpcodeAcquire &&
          raw_frames.headers[0U].request_id == 2U &&
          raw_frames.headers[0U].kind == wire::Kind::response);
    const auto raw_lease = wire::decode_acquire_response(
        ByteView{raw_frames.payloads[0U].data(), raw_frames.payload_sizes[0U]});
    CHECK(raw_lease);
    auto leased_status = empty_request(*first, MessageType::STATUS);
    CHECK(leased_status);
    const auto leased_snapshot = wire::decode_status_response(leased_status.value().view());
    CHECK(leased_snapshot && leased_snapshot.value().receivers.value.items[1].leased);
    raw_frames.clear();
    std::array<std::uint8_t, 24U> release_payload{};
    const auto release_size = wire::encode_release_request(
        wire::ReleaseRequest{raw_epoch, raw_lease.value().lease_id},
        MutableByteView{release_payload.data(), release_payload.size()});
    CHECK(release_size && send_raw_request(
        raw, MessageType::RELEASE, 3U,
        ByteView{release_payload.data(), release_size.value()}));
    CHECK(read_raw_frames(raw, raw_framer, raw_frames, 1U) &&
          raw_frames.headers[0U].opcode == wire::kOpcodeRelease &&
          raw_frames.headers[0U].kind == wire::Kind::response &&
          raw_frames.headers[0U].request_id == 3U);
    auto released_status = empty_request(*first, MessageType::STATUS);
    CHECK(released_status);
    const auto released_snapshot = wire::decode_status_response(released_status.value().view());
    CHECK(released_snapshot &&
          !released_snapshot.value().receivers.value.items[1].leased &&
          released_snapshot.value().receivers.value.items[1].state == wire::ReceiverState::free &&
          released_snapshot.value().snapshot.sequence > leased_snapshot.value().snapshot.sequence);
    raw.close();

    wire::TuneRequest valid_tune;
    valid_tune.epoch = first->client->epoch;
    valid_tune.lease_id = terrestrial_lease.value().lease_id;
    valid_tune.frequency_hz = 500000000;
    valid_tune.timeout_ms = 100;
    auto invalid_tune = WireClient::encode(valid_tune, &wire::encode_tune_request);
    CHECK(invalid_tune.size() > 4);
    invalid_tune[invalid_tune.size() - 4] = 99;
    const auto invalid_semantics = first->exchange(wire::kOpcodeTune, invalid_tune);
    CHECK(!invalid_semantics && invalid_semantics.error() == Error::INVALID_ARGUMENT);
    CHECK(empty_request(*first, MessageType::STATUS));

    const auto release_result = request(
        *first, MessageType::RELEASE,
        LeaseRequestPayload{terrestrial_lease.value().lease_id});
    CHECK(release_result);
    const auto expired_stats = first->record(wire::kOpcodeStatus,
        wire::StatusRequest{first->client->epoch,
            {wire::ScopeKind::stream, first->starts.at(terrestrial_lease.value().lease_id).stream_id}},
        &wire::encode_status_request);
    CHECK(!expired_stats && expired_stats.error() == Error::NOT_FOUND);
    const auto release_satellite = request(
        *first, MessageType::RELEASE,
        LeaseRequestPayload{satellite_lease.value().lease_id});
    CHECK(release_satellite);

    first->close();
    std::this_thread::sleep_for(std::chrono::milliseconds(100U));
    const auto reacquired = request(*second, MessageType::ACQUIRE,
                                    AcquireRequestPayload{2U});
    CHECK(reacquired);
    const auto reacquired_lease = wire::decode_acquire_response(
        ByteView{reacquired.value().payload.data(), reacquired.value().payload.size()});
    CHECK(reacquired_lease);
    const auto reacquired_tune = request(
        *second, MessageType::TUNE,
        TuneRequestPayload{reacquired_lease.value().lease_id, System::ISDB_T,
                           500000U, 0xffffU, 0xffffU, 6000000U, 0U, 100U});
    CHECK(reacquired_tune);
    const auto start_stream = request(
        *second, MessageType::START_STREAM,
        LeaseRequestPayload{reacquired_lease.value().lease_id});
    CHECK(start_stream);
    const auto duplicate_start = request(
        *second, MessageType::START_STREAM,
        LeaseRequestPayload{reacquired_lease.value().lease_id});
    const auto stop_stream = request(
        *second, MessageType::STOP_STREAM,
        LeaseRequestPayload{reacquired_lease.value().lease_id});
    CHECK(!duplicate_start && duplicate_start.error() == Error::BUSY && stop_stream);
    const auto armed_stop = wire::decode_stop_response(stop_stream.value().view());
    CHECK(armed_stop && armed_stop.value().quiesced);
    const auto stats = empty_request(*second, MessageType::STATUS);
    CHECK(stats);
    const auto stopped_status = wire::decode_status_response(stats.value().view());
    CHECK(stopped_status &&
          stopped_status.value().receivers.value.items[2].state == wire::ReceiverState::tuned &&
          !stopped_status.value().receivers.value.items[2].stream.present);
    CHECK(request(*second, MessageType::RELEASE,
                  LeaseRequestPayload{reacquired_lease.value().lease_id}));

    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    const auto shutdown = server->shutdown();
    CHECK(shutdown);
    CHECK(tuner_backend.close_calls[2U] >= 2U && tuner_backend.close_calls[0U] >= 1U);
    return true;
}

bool test_control_server_lanes_are_independent()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000963";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time);
    auto server_result = create_typed_server(endpoint, card_service, tuner_service, serial);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{20U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });

    ServerThreadGuard server_guard(stop, server_thread, *server);
    auto slow_result = TypedControl::connect(endpoint, 0U, Timeout{2000U});
    CHECK(slow_result);
    std::unique_ptr<TypedControl> slow = std::move(slow_result.value());
    tuner_backend.block_receiver_zero.store(true);
    std::atomic<bool> slow_done{false};
    bool slow_ok = false;
    std::uint64_t slow_lease = 0U;
    std::thread slow_request([&]() {
        const auto acquired = request(*slow, MessageType::ACQUIRE,
                                      AcquireRequestPayload{0U});
        slow_ok = static_cast<bool>(acquired);
        if (acquired) {
            const auto payload = wire::decode_acquire_response(
                ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
            if (payload) slow_lease = payload.value().lease_id;
        }
        slow_done.store(true);
    });
    BlockedOperationGuard blocked_acquire_guard(
        tuner_backend.block_receiver_zero, tuner_backend.receiver_zero_condition,
        slow_request, stop, server_thread, *server);
    bool started = false;
    {
        std::unique_lock<std::mutex> lock(tuner_backend.receiver_zero_mutex);
        started = tuner_backend.receiver_zero_condition.wait_for(
            lock, std::chrono::seconds(10),
            [&]() { return tuner_backend.receiver_zero_started.load(); });
    }
    if (!started) {
        return false;
    }

    auto fast_result = TypedControl::connect(endpoint, kCapabilityCard, Timeout{2000U});
    CHECK(fast_result);
    std::unique_ptr<TypedControl> fast = std::move(fast_result.value());
    CHECK(empty_request(*fast, MessageType::LIST));
    const auto dev2 = request(*fast, MessageType::ACQUIRE,
                              AcquireRequestPayload{4U});
    CHECK(dev2);
    const auto dev2_payload = wire::decode_acquire_response(
        ByteView{dev2.value().payload.data(), dev2.value().payload.size()});
    CHECK(dev2_payload && request(*fast, MessageType::RELEASE,
                                  LeaseRequestPayload{dev2_payload.value().lease_id}));
    const auto card = request(*fast, MessageType::CARD_CONNECT,
                              CardConnectRequestPayload{ShareMode::shared});
    CHECK(card);
    const auto card_payload = wire::decode_card_session_result(
        ByteView{card.value().payload.data(), card.value().payload.size()});
    CHECK(card_payload && request(
        *fast, MessageType::CARD_DISCONNECT,
        CardDispositionRequestPayload{card_payload.value().card_handle,
                                      Disposition::leave}));

    tuner_backend.block_receiver_zero.store(false);
    slow_request.join();
    CHECK(slow_done.load() && slow_ok && slow_lease != 0U);
    CHECK(request(*slow, MessageType::RELEASE, LeaseRequestPayload{slow_lease}));
    slow->close();
    fast->close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    blocked_acquire_guard.dismiss();
    return true;
}

bool test_control_server_disconnect_during_blocked_acquire()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000964";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time);
    auto server_result = create_typed_server(endpoint, card_service, tuner_service, serial);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{20U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto raw_result = SocketStream::connect(endpoint, Timeout{2000U});
    CHECK(raw_result);
    SocketStream raw = std::move(raw_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> raw_storage{};
    wire::FrameReceiver raw_framer(MutableByteView{raw_storage.data(), raw_storage.size()});
    wire::Uuid raw_epoch{};
    CHECK(raw_hello(raw, raw_framer, raw_epoch));

    tuner_backend.block_receiver_zero.store(true);
    std::atomic<bool> request_done{false};
    std::atomic<bool> response_seen{false};
    std::thread request_thread([&]() {
        const auto payload = WireClient::encode(wire::AcquireRequest{raw_epoch, 0},
                                                &wire::encode_acquire_request);
        const auto sent = send_raw_request(
            raw, MessageType::ACQUIRE, 2U, ByteView{payload.data(), payload.size()});
        if (sent) {
            FrameCollector frames;
            if (read_raw_frames(raw, raw_framer, frames, 1U)) response_seen.store(true);
        }
        request_done.store(true);
    });
    BlockedOperationGuard blocked_guard(
        tuner_backend.block_receiver_zero, tuner_backend.receiver_zero_condition,
        request_thread, stop, server_thread, *server);
    {
        std::unique_lock<std::mutex> lock(tuner_backend.receiver_zero_mutex);
        CHECK(tuner_backend.receiver_zero_condition.wait_for(
            lock, std::chrono::seconds(10),
            [&]() { return tuner_backend.receiver_zero_started.load(); }));
    }

    // shutdown() is an OS-level operation on the borrowed descriptor; the
    // SocketStream object remains owned by request_thread, avoiding a C++
    // data race while making the disconnect happen before unblocking open.
    CHECK(::shutdown(raw.native_handle(), SHUT_RDWR) == 0);
    blocked_guard.unblock_and_join();
    CHECK(request_done.load() && !response_seen.load());

    auto replacement_result = TypedControl::connect(
        endpoint, 0U, Timeout{2000U});
    CHECK(replacement_result);
    std::unique_ptr<TypedControl> replacement =
        std::move(replacement_result.value());
    Result<WireReply> acquired = Result<WireReply>::failure(Error::BUSY);
    for (std::size_t attempt = 0U; attempt < 500U; ++attempt) {
        acquired = request(*replacement, MessageType::ACQUIRE,
                           AcquireRequestPayload{0U});
        if (acquired) break;
        CHECK(acquired.error() == Error::BUSY);
        std::this_thread::sleep_for(std::chrono::milliseconds(10U));
    }
    CHECK(acquired);
    const auto lease = wire::decode_acquire_response(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(lease && request(*replacement, MessageType::RELEASE,
                           LeaseRequestPayload{lease.value().lease_id}));
    replacement->close();
    raw.close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    blocked_guard.dismiss();
    server_guard.dismiss();
    return true;
}

bool test_control_server_disconnect_during_blocked_tune()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000965";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time);
    auto server_result = create_typed_server(endpoint, card_service, tuner_service, serial);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{20U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto raw_result = SocketStream::connect(endpoint, Timeout{2000U});
    CHECK(raw_result);
    SocketStream raw = std::move(raw_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> raw_storage{};
    wire::FrameReceiver raw_framer(MutableByteView{raw_storage.data(), raw_storage.size()});
    wire::Uuid raw_epoch{};
    CHECK(raw_hello(raw, raw_framer, raw_epoch));
    const auto acquire_payload = WireClient::encode(wire::AcquireRequest{raw_epoch, 0},
                                                   &wire::encode_acquire_request);
    CHECK(send_raw_request(raw, MessageType::ACQUIRE, 2U,
                           ByteView{acquire_payload.data(), acquire_payload.size()}));
    FrameCollector acquire_frames;
    CHECK(read_raw_frames(raw, raw_framer, acquire_frames, 1U));
    const auto lease = wire::decode_acquire_response(
        ByteView{acquire_frames.payloads[0U].data(), acquire_frames.payload_sizes[0U]});
    CHECK(lease);

    tuner_backend.block_tune_receiver_zero.store(true);
    std::atomic<bool> request_done{false};
    std::atomic<bool> response_seen{false};
    std::thread request_thread([&]() {
        std::array<std::uint8_t, kMaxControlPayload> payload{};
        wire::TuneRequest tune;
        tune.epoch = raw_epoch;
        tune.lease_id = lease.value().lease_id;
        tune.system = wire::System::isdb_s;
        tune.frequency_hz = 1200000000;
        tune.selection = wire::Selection::slot;
        tune.selection_value = 1;
        tune.timeout_ms = 100;
        const auto payload_size = wire::encode_tune_request(tune,
            MutableByteView{payload.data(), payload.size()});
        const auto sent = payload_size && send_raw_request(
            raw, MessageType::TUNE, 3U,
            ByteView{payload.data(), payload_size.value()});
        if (sent) {
            FrameCollector frames;
            if (read_raw_frames(raw, raw_framer, frames, 1U)) response_seen.store(true);
        }
        request_done.store(true);
    });
    BlockedOperationGuard blocked_guard(
        tuner_backend.block_tune_receiver_zero,
        tuner_backend.tune_receiver_zero_condition, request_thread, stop,
        server_thread, *server);
    {
        std::unique_lock<std::mutex> lock(tuner_backend.tune_receiver_zero_mutex);
        CHECK(tuner_backend.tune_receiver_zero_condition.wait_for(
            lock, std::chrono::seconds(10),
            [&]() { return tuner_backend.tune_receiver_zero_started.load(); }));
    }
    CHECK(::shutdown(raw.native_handle(), SHUT_RDWR) == 0);
    blocked_guard.unblock_and_join();
    CHECK(request_done.load() && !response_seen.load());

    auto replacement_result = TypedControl::connect(
        endpoint, 0U, Timeout{2000U});
    CHECK(replacement_result);
    std::unique_ptr<TypedControl> replacement =
        std::move(replacement_result.value());
    Result<WireReply> acquired = Result<WireReply>::failure(Error::BUSY);
    for (std::size_t attempt = 0U; attempt < 500U; ++attempt) {
        acquired = request(*replacement, MessageType::ACQUIRE,
                           AcquireRequestPayload{0U});
        if (acquired) break;
        CHECK(acquired.error() == Error::BUSY);
        std::this_thread::sleep_for(std::chrono::milliseconds(10U));
    }
    CHECK(acquired);
    const auto replacement_lease = wire::decode_acquire_response(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(replacement_lease &&
          replacement_lease.value().lease_id != lease.value().lease_id);
    CHECK(request(*replacement, MessageType::RELEASE,
                  LeaseRequestPayload{replacement_lease.value().lease_id}));
    replacement->close();
    raw.close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    blocked_guard.dismiss();
    server_guard.dismiss();
    return true;
}

bool test_stream_endpoint_attach_and_stream_end()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000964";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    auto server_result = create_typed_server(endpoint, card_service, tuner_service,
                                             serial, &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    const std::string stream_path = server->stream_endpoint_path();
    struct stat status {};
    CHECK(::stat(stream_path.c_str(), &status) == 0 &&
          (status.st_mode & 0777U) == 0600U);

    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{10U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto control_result = TypedControl::connect(
        endpoint, kCapabilityStreamStats, Timeout{2000U});
    CHECK(control_result);
    std::unique_ptr<TypedControl> control = std::move(control_result.value());
    const auto acquired = request(*control, MessageType::ACQUIRE,
                                  AcquireRequestPayload{2U});
    CHECK(acquired);
    const auto lease = wire::decode_acquire_response(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(lease);
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{lease.value().lease_id}));

    const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                         kStreamEndpointName,
                                         EndpointAccess::private_user};
    auto stream_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(stream_result);
    SocketStream stream = std::move(stream_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> storage{};
    wire::FrameReceiver framer(MutableByteView{storage.data(), storage.size()});
    std::array<std::uint8_t, kMaxControlPayload> attach_payload{};
    {
        std::lock_guard<std::mutex> lock(stream_control.mutex);
        stream_control.data_packet.fill(0xa5U);
        stream_control.data_packet[0U] = 0x47U;
        stream_control.emit_data = true;
    }
    const auto attach_size = wire::encode_attach_request(
        control->attachment(lease.value().lease_id),
        MutableByteView{attach_payload.data(), attach_payload.size()});
    CHECK(attach_size && send_raw_request(
        stream, MessageType::ATTACH_STREAM, 4U,
        ByteView{attach_payload.data(), attach_size.value()}));
    FrameCollector frames;
    CHECK(read_raw_frames(stream, framer, frames, 1U));
    std::size_t attach_index = frames.count;
    std::size_t data_index = frames.count;
    for (std::size_t index = 0U; index < frames.count; ++index) {
        if (frames.headers[index].opcode == wire::kOpcodeAttachStream &&
            frames.headers[index].kind == wire::Kind::response &&
            frames.headers[index].request_id == 4U) {
            attach_index = index;
        }
        if (frames.headers[index].opcode == wire::kOpcodeTsDataEvent &&
            frames.headers[index].kind == wire::Kind::event) {
            data_index = index;
        }
    }
    CHECK(attach_index < frames.count);

    auto wrong_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(wrong_result);
    SocketStream wrong = std::move(wrong_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> wrong_storage{};
    wire::FrameReceiver wrong_framer(MutableByteView{wrong_storage.data(), wrong_storage.size()});
    auto wrong_payload = control->attachment(lease.value().lease_id);
    wrong_payload.attach_token[0U] ^= 0xffU;
    std::array<std::uint8_t, kMaxControlPayload> wrong_encoded{};
    const auto wrong_size = wire::encode_attach_request(
        wrong_payload, MutableByteView{wrong_encoded.data(), wrong_encoded.size()});
    CHECK(wrong_size && send_raw_request(
        wrong, MessageType::ATTACH_STREAM, 7U,
        ByteView{wrong_encoded.data(), wrong_size.value()}));
    FrameCollector wrong_frames;
    CHECK(read_raw_frames(wrong, wrong_framer, wrong_frames, 1U));
    const auto wrong_error = wire::decode_error_response(
        ByteView{wrong_frames.payloads[0U].data(), wrong_frames.payload_sizes[0U]});
    CHECK(wrong_frames.headers[0U].kind == wire::Kind::error_response &&
          wrong_error && wrong_error.value().error.code == static_cast<std::uint32_t>(Error::NOT_FOUND));
    wrong.close();

    // The acquire nonce is one-use: a second data connection presenting the
    // same lease/nonce must not attach the already-active session.
    auto reused_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(reused_result);
    SocketStream reused = std::move(reused_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> reused_storage{};
    wire::FrameReceiver reused_framer(
        MutableByteView{reused_storage.data(), reused_storage.size()});
    CHECK(send_raw_request(reused, MessageType::ATTACH_STREAM, 8U,
                           ByteView{attach_payload.data(), attach_size.value()}));
    FrameCollector reused_frames;
    CHECK(read_raw_frames(reused, reused_framer, reused_frames, 1U));
    const auto reused_error = wire::decode_error_response(
        ByteView{reused_frames.payloads[0U].data(),
                 reused_frames.payload_sizes[0U]});
    CHECK(reused_frames.headers[0U].kind == wire::Kind::error_response &&
          reused_error && reused_error.value().error.code == static_cast<std::uint32_t>(Error::NOT_FOUND));
    reused.close();

    if (data_index == frames.count) {
        frames.clear();
        CHECK(read_raw_frames(stream, framer, frames, 1U));
        data_index = 0U;
    }
    CHECK(data_index < frames.count &&
          frames.headers[data_index].opcode == wire::kOpcodeTsDataEvent &&
          frames.headers[data_index].kind == wire::Kind::event);
    const auto data = wire::decode_ts_data(
        ByteView{frames.payloads[data_index].data(), frames.payload_sizes[data_index]});
    CHECK(data && data.value().sequence == 0U &&
          data.value().cumulative_queue_dropped_packets == 4U &&
          data.value().packets.size == stream_control.data_packet.size() &&
          data.value().packets.data[0U] == 0x47U &&
          data.value().packets.data[187U] == 0xa5U);

    const auto stopped = request(*control, MessageType::STOP_STREAM,
                                 LeaseRequestPayload{lease.value().lease_id});
    CHECK(stopped);
    frames.clear();
    CHECK(read_raw_frames(stream, framer, frames, 1U) &&
          frames.headers[0U].opcode == wire::kOpcodeStreamEndEvent &&
          frames.headers[0U].kind == wire::Kind::event);
    const auto end = wire::decode_stream_end_event(
        ByteView{frames.payloads[0U].data(), frames.payload_sizes[0U]});
    CHECK(end && end_error_is(end.value(), ErrorCode::OK) &&
          end.value().final.reason == wire::StreamEndReason::stopped);
    CHECK(request(*control, MessageType::RELEASE,
                  LeaseRequestPayload{lease.value().lease_id}));

    stream.close();
    control->close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    server_guard.dismiss();
    return true;
}

bool test_stream_endpoint_terminal_mapping()
{
    const std::array<std::pair<TunerStreamTerminal, ErrorCode>, 6U> cases{{
        {TunerStreamTerminal::stopped, ErrorCode::OK},
        {TunerStreamTerminal::slow_consumer, ErrorCode::SLOW_CONSUMER},
        {TunerStreamTerminal::usb_error, ErrorCode::USB_IO},
        {TunerStreamTerminal::disconnected, ErrorCode::DISCONNECTED},
        {TunerStreamTerminal::sync_error, ErrorCode::PROTOCOL_ERROR},
        {TunerStreamTerminal::bridge_fatal, ErrorCode::INTERNAL},
    }};

    for (const auto& terminal_case : cases) {
        TempRuntime runtime;
        CHECK(runtime.valid());
        constexpr const char* serial = "00001205000965";
        const EndpointConfig endpoint{runtime.path().c_str(), serial,
                                      kControlEndpointName,
                                      EndpointAccess::private_user};
        Backend card_backend;
        Session card_session;
        CardService card_service(card_backend, card_session);
        TunerBackend tuner_backend;
        TunerNonce tuner_nonce;
        TunerTime tuner_time;
        ControlStream stream_control;
        TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                                   nullptr, nullptr, &stream_control);
        auto server_result = create_typed_server(endpoint, card_service, tuner_service,
                                                 serial, &stream_control);
        CHECK(server_result);
        std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
        std::atomic<bool> stop{false};
        std::atomic<bool> server_ok{true};
        std::thread server_thread([&]() {
            while (!stop.load()) {
                const auto polled = server->poll_once(Timeout{10U});
                if (!polled) {
                    server_ok.store(false);
                    return;
                }
            }
        });
        ServerThreadGuard server_guard(stop, server_thread, *server);

        auto control_result = TypedControl::connect(
            endpoint, kCapabilityStreamStats, Timeout{2000U});
        CHECK(control_result);
        std::unique_ptr<TypedControl> control = std::move(control_result.value());
        const auto acquired = request(*control, MessageType::ACQUIRE,
                                      AcquireRequestPayload{2U});
        CHECK(acquired);
        const auto lease = wire::decode_acquire_response(
            ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
        CHECK(lease);
        CHECK(request(*control, MessageType::TUNE,
                      TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                         500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                         100U}));
        CHECK(request(*control, MessageType::START_STREAM,
                      LeaseRequestPayload{lease.value().lease_id}));

        const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                             kStreamEndpointName,
                                             EndpointAccess::private_user};
        auto stream_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
        CHECK(stream_result);
        SocketStream stream = std::move(stream_result.value());
        std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> storage{};
        wire::FrameReceiver framer(MutableByteView{storage.data(), storage.size()});
        std::array<std::uint8_t, kMaxControlPayload> payload{};
        const auto payload_size = wire::encode_attach_request(
        control->attachment(lease.value().lease_id),
            MutableByteView{payload.data(), payload.size()});
        CHECK(payload_size && send_raw_request(
            stream, MessageType::ATTACH_STREAM, 4U,
            ByteView{payload.data(), payload_size.value()}));
        FrameCollector frames;
        CHECK(read_raw_frames(stream, framer, frames, 1U));
        CHECK(frames.headers[0U].opcode == wire::kOpcodeAttachStream &&
              frames.headers[0U].kind == wire::Kind::response);
        {
            std::lock_guard<std::mutex> lock(stream_control.mutex);
            stream_control.counters = TunerStreamCounters{31U, 5828U, 7U, 11U,
                                                            13U, 17U, 19U, 23U};
            stream_control.final_terminal = terminal_case.first;
        }
        const auto stopped = request(*control, MessageType::STOP_STREAM,
                                     LeaseRequestPayload{lease.value().lease_id});
        CHECK(stopped);
        frames.clear();
        CHECK(read_raw_frames(stream, framer, frames, 1U));
        CHECK(frames.headers[0U].opcode == wire::kOpcodeStreamEndEvent &&
              frames.headers[0U].kind == wire::Kind::event);
        const auto end = wire::decode_stream_end_event(
            ByteView{frames.payloads[0U].data(), frames.payload_sizes[0U]});
        CHECK(end && end_error_is(end.value(), terminal_case.second) &&
              counters_are(end.value().final.statistics, 31U, 17U));
        const auto expected_reason = terminal_case.first == TunerStreamTerminal::stopped ?
            wire::StreamEndReason::stopped :
            terminal_case.first == TunerStreamTerminal::slow_consumer ?
            wire::StreamEndReason::slow_consumer :
            (terminal_case.first == TunerStreamTerminal::usb_error ||
             terminal_case.first == TunerStreamTerminal::disconnected) ?
            wire::StreamEndReason::device_lost : wire::StreamEndReason::backend_error;
        CHECK(end.value().final.reason == expected_reason);
        const auto native_final = stream_control.final_snapshot(stream_control.final_identity);
        CHECK(native_final && native_final.value().terminal ==
              static_cast<std::uint8_t>(terminal_case.first) &&
              native_final.value().counters.packets == 31U &&
              native_final.value().counters.bytes == 5828U &&
              native_final.value().counters.sync_errors == 7U &&
              native_final.value().counters.tei_packets == 11U &&
              native_final.value().counters.continuity_errors == 13U &&
              native_final.value().counters.queue_drops == 17U &&
              native_final.value().counters.usb_errors == 19U &&
              native_final.value().counters.empty_intervals == 23U);
        const auto released = request(
            *control, MessageType::RELEASE,
            LeaseRequestPayload{lease.value().lease_id});
        if (terminal_case.first == TunerStreamTerminal::disconnected) {
            // STREAM_END retains transport loss. Typed RELEASE independently
            // reports the successful physical cleanup performed by this fake.
            CHECK(released && tuner_backend.close_calls[2U] == 1U);
            const auto cleanup = wire::decode_release_response(released.value().view());
            CHECK(cleanup && cleanup.value().released && cleanup.value().quiesced &&
                  !cleanup.value().error.present);
        } else {
            CHECK(released);
        }
        stream.close();
        control->close();
        stop.store(true);
        server_thread.join();
        CHECK(server_ok.load());
        CHECK(server->shutdown());
        server_guard.dismiss();
    }
    return true;
}

bool test_stream_attach_submit_failure_cleanup()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000966";
    const EndpointConfig endpoint{runtime.path().c_str(), serial,
                                  kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    ControlWorkerStartupOptions worker_options{};
    worker_options.fail_tuner_attach_submit = true;
    ControlWorkerLanes::set_test_startup_options(&worker_options);
    auto server_result = create_typed_server(endpoint, card_service, tuner_service,
                                             serial, &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{10U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto control_result = TypedControl::connect(
        endpoint, kCapabilityStreamStats, Timeout{2000U});
    CHECK(control_result);
    std::unique_ptr<TypedControl> control = std::move(control_result.value());
    const auto acquired = request(*control, MessageType::ACQUIRE,
                                  AcquireRequestPayload{2U});
    CHECK(acquired);
    const auto lease = wire::decode_acquire_response(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(lease);
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{lease.value().lease_id}));

    const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                         kStreamEndpointName,
                                         EndpointAccess::private_user};
    auto stream_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(stream_result);
    SocketStream stream = std::move(stream_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> storage{};
    wire::FrameReceiver framer(MutableByteView{storage.data(), storage.size()});
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    const auto payload_size = wire::encode_attach_request(
        control->attachment(lease.value().lease_id),
        MutableByteView{payload.data(), payload.size()});
    CHECK(payload_size && send_raw_request(
        stream, MessageType::ATTACH_STREAM, 4U,
        ByteView{payload.data(), payload_size.value()}));
    FrameCollector frames;
    CHECK(read_raw_frames(stream, framer, frames, 1U));
    const auto error = wire::decode_error_response(
        ByteView{frames.payloads[0U].data(), frames.payload_sizes[0U]});
    CHECK(frames.headers[0U].kind == wire::Kind::error_response && error &&
          error.value().error.code == static_cast<std::uint32_t>(Error::BUSY));
    std::array<std::uint8_t, 1U> closed_buffer{};
    const auto closed = stream.read_some(
        MutableByteView{closed_buffer.data(), closed_buffer.size()}, Timeout{2000U});
    CHECK(!closed && closed.error() == Error::DISCONNECTED);
    CHECK(request(*control, MessageType::RELEASE,
                  LeaseRequestPayload{lease.value().lease_id}));
    stream.close();
    control->close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    server_guard.dismiss();
    return true;
}

bool test_stream_hup_before_attach_completion_cleanup()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000967";
    const EndpointConfig endpoint{runtime.path().c_str(), serial,
                                  kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    AttachCompletionBarrier completion_barrier;
    ControlWorkerStartupOptions worker_options{};
    worker_options.before_tuner_attach_completion =
        &AttachCompletionBarrier::before_completion;
    worker_options.before_tuner_attach_completion_context = &completion_barrier;
    ControlWorkerLanes::set_test_startup_options(&worker_options);
    auto server_result = create_typed_server(endpoint, card_service, tuner_service,
                                             serial, &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{10U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);
    AttachCompletionReleaseGuard completion_guard(completion_barrier);

    auto control_result = TypedControl::connect(
        endpoint, kCapabilityStreamStats, Timeout{2000U});
    CHECK(control_result);
    std::unique_ptr<TypedControl> control = std::move(control_result.value());
    const auto acquired = request(*control, MessageType::ACQUIRE,
                                  AcquireRequestPayload{2U});
    CHECK(acquired);
    const auto lease = wire::decode_acquire_response(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(lease);
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{lease.value().lease_id}));

    const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                         kStreamEndpointName,
                                         EndpointAccess::private_user};
    auto stream_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(stream_result);
    SocketStream stream = std::move(stream_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> storage{};
    wire::FrameReceiver framer(MutableByteView{storage.data(), storage.size()});
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    const auto payload_size = wire::encode_attach_request(
        control->attachment(lease.value().lease_id),
        MutableByteView{payload.data(), payload.size()});
    CHECK(payload_size && send_raw_request(
        stream, MessageType::ATTACH_STREAM, 4U,
        ByteView{payload.data(), payload_size.value()}));
    CHECK(completion_barrier.wait_until_entered());

    // The worker has already completed TunerService::attach_stream, but its
    // completion is held before publication.  HUP therefore exercises the
    // retired waiting_attach path rather than the ordinary live path.
    stream.close();
    completion_barrier.release();
    CHECK(stream_control.wait_for_detaches(1U));
    CHECK(request(*control, MessageType::RELEASE,
                  LeaseRequestPayload{lease.value().lease_id}));
    CHECK(tuner_backend.start_capture_calls[2U] == 1U &&
          tuner_backend.stop_capture_calls[2U] == 1U &&
          tuner_backend.close_calls[2U] == 1U &&
          stream_control.detach_calls == 1U);
    control->close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    completion_guard.dismiss();
    server_guard.dismiss();
    return true;
}

bool test_stream_endpoint_fragmented_and_duplicate()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000968";
    const EndpointConfig endpoint{runtime.path().c_str(), serial,
                                  kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    StreamServerObservation observation;
    StreamServerHookGuard hook_guard(observation);
    auto server_result = create_typed_server(endpoint, card_service, tuner_service,
                                             serial, &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{10U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto control_result = TypedControl::connect(
        endpoint, kCapabilityStreamStats, Timeout{2000U});
    CHECK(control_result);
    std::unique_ptr<TypedControl> control = std::move(control_result.value());
    const auto acquired = request(*control, MessageType::ACQUIRE,
                                  AcquireRequestPayload{2U});
    CHECK(acquired);
    const auto lease = wire::decode_acquire_response(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(lease);
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{lease.value().lease_id}));

    const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                         kStreamEndpointName,
                                         EndpointAccess::private_user};
    auto stream_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(stream_result);
    SocketStream stream = std::move(stream_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> storage{};
    wire::FrameReceiver framer(MutableByteView{storage.data(), storage.size()});
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    const auto payload_size = wire::encode_attach_request(
        control->attachment(lease.value().lease_id),
        MutableByteView{payload.data(), payload.size()});
    CHECK(payload_size);
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> frame{};
    wire::Header attach_header;
    attach_header.opcode = wire::kOpcodeAttachStream;
    attach_header.request_id = 4;
    attach_header.payload_length = static_cast<std::uint32_t>(payload_size.value());
    const auto frame_size = wire::encode_frame(attach_header,
        ByteView{payload.data(), payload_size.value()},
        MutableByteView{frame.data(), frame.size()});
    CHECK(frame_size && frame_size.value() > kFrameHeaderSize);
    const std::size_t first_bytes = kFrameHeaderSize - 1U;
    CHECK(send_socket_bytes(stream, frame.data(), first_bytes));
    CHECK(observation.wait_for_partial());
    CHECK(send_socket_bytes(stream, frame.data() + first_bytes,
                            frame_size.value() - first_bytes));
    FrameCollector frames;
    CHECK(read_raw_frames(stream, framer, frames, 1U));
    CHECK(frames.headers[0U].opcode == wire::kOpcodeAttachStream &&
          frames.headers[0U].kind == wire::Kind::response);

    // A stream connection accepts exactly one inbound frame.  A second
    // ATTACH is a protocol violation and must close the connection while
    // still executing the ordinary exact-identity cleanup.
    CHECK(send_raw_request(stream, MessageType::ATTACH_STREAM, 1U,
                           ByteView{payload.data(), payload_size.value()}));
    std::array<std::uint8_t, 1U> closed_buffer{};
    const auto closed = stream.read_some(
        MutableByteView{closed_buffer.data(), closed_buffer.size()}, Timeout{2000U});
    CHECK(!closed && closed.error() == Error::DISCONNECTED);
    CHECK(stream_control.wait_for_detaches(1U));
    CHECK(request(*control, MessageType::RELEASE,
                  LeaseRequestPayload{lease.value().lease_id}));
    stream.close();
    control->close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    server_guard.dismiss();
    return true;
}

bool test_stream_endpoint_capacity_and_retired_reaping()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000969";
    const EndpointConfig endpoint{runtime.path().c_str(), serial,
                                  kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    StreamServerObservation observation;
    StreamServerHookGuard hook_guard(observation);
    auto server_result = create_typed_server(endpoint, card_service, tuner_service,
                                             serial, &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{10U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto control_result = TypedControl::connect(
        endpoint, kCapabilityStreamStats, Timeout{2000U});
    CHECK(control_result);
    std::unique_ptr<TypedControl> control = std::move(control_result.value());
    const auto acquired = request(*control, MessageType::ACQUIRE,
                                  AcquireRequestPayload{2U});
    CHECK(acquired);
    const auto lease = wire::decode_acquire_response(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(lease);
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{lease.value().lease_id}));
    const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                         kStreamEndpointName,
                                         EndpointAccess::private_user};
    auto attached_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(attached_result);
    SocketStream attached = std::move(attached_result.value());
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    const auto payload_size = wire::encode_attach_request(
        control->attachment(lease.value().lease_id),
        MutableByteView{payload.data(), payload.size()});
    CHECK(payload_size && send_raw_request(
        attached, MessageType::ATTACH_STREAM, 4U,
        ByteView{payload.data(), payload_size.value()}));
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> attached_storage{};
    wire::FrameReceiver attached_framer(
        MutableByteView{attached_storage.data(), attached_storage.size()});
    FrameCollector attached_frames;
    CHECK(read_raw_frames(attached, attached_framer, attached_frames, 1U));
    CHECK(attached_frames.headers[0U].kind == wire::Kind::response);

    // kMaxStreamConnections is a private production bound; this test mirrors
    // its exact supported value and observes acceptance through the private
    // test hook rather than guessing from connect() backlog state.
    constexpr std::size_t kSupportedDataClients = 32U;
    std::vector<SocketStream> idle_clients;
    idle_clients.reserve(kSupportedDataClients - 1U);
    for (std::size_t index = 1U; index < kSupportedDataClients; ++index) {
        auto idle_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
        CHECK(idle_result);
        idle_clients.emplace_back(std::move(idle_result.value()));
        CHECK(observation.wait_for_capacity(index + 1U, 0U));
    }

    auto overflow_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(overflow_result);
    SocketStream overflow = std::move(overflow_result.value());
    std::array<std::uint8_t, 1U> overflow_buffer{};
    const auto overflow_closed = overflow.read_some(
        MutableByteView{overflow_buffer.data(), overflow_buffer.size()}, Timeout{2000U});
    CHECK(!overflow_closed && overflow_closed.error() == Error::DISCONNECTED);

    // This removal needs a retired exact-identity owner before its detach
    // completion arrives.  Capacity remains full until that owner is reaped.
    attached.close();
    CHECK(observation.wait_for_retired_owner(kSupportedDataClients - 1U));
    CHECK(stream_control.wait_for_detaches(1U));
    CHECK(observation.wait_for_capacity(kSupportedDataClients - 1U, 0U));

    auto replacement_result = SocketStream::connect(
        stream_endpoint, Timeout{2000U});
    CHECK(replacement_result);
    SocketStream replacement = std::move(replacement_result.value());
    CHECK(observation.wait_for_capacity(kSupportedDataClients, 0U));
    replacement.close();
    overflow.close();
    for (SocketStream& idle : idle_clients) idle.close();
    CHECK(request(*control, MessageType::RELEASE,
                  LeaseRequestPayload{lease.value().lease_id}));
    control->close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    server_guard.dismiss();
    return true;
}

bool test_stream_endpoint_nonreading_peer_isolated()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000970";
    const EndpointConfig endpoint{runtime.path().c_str(), serial,
                                  kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    {
        std::lock_guard<std::mutex> lock(stream_control.mutex);
        stream_control.data_packet.fill(0x5aU);
        stream_control.data_packet[0U] = 0x47U;
        stream_control.continuous_data = true;
    }
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    StreamServerObservation observation;
    StreamServerHookGuard hook_guard(observation);
    auto server_result = create_typed_server(endpoint, card_service, tuner_service,
                                             serial, &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{10U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto control_result = TypedControl::connect(
        endpoint, kCapabilityStreamStats, Timeout{2000U});
    CHECK(control_result);
    std::unique_ptr<TypedControl> control = std::move(control_result.value());
    const auto first_acquired = request(*control, MessageType::ACQUIRE,
                                        AcquireRequestPayload{2U});
    const auto second_acquired = request(*control, MessageType::ACQUIRE,
                                         AcquireRequestPayload{3U});
    CHECK(first_acquired && second_acquired);
    const auto first_lease = wire::decode_acquire_response(
        ByteView{first_acquired.value().payload.data(),
                 first_acquired.value().payload.size()});
    const auto second_lease = wire::decode_acquire_response(
        ByteView{second_acquired.value().payload.data(),
                 second_acquired.value().payload.size()});
    CHECK(first_lease && second_lease);
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{first_lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{second_lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{first_lease.value().lease_id}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{second_lease.value().lease_id}));

    const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                         kStreamEndpointName,
                                         EndpointAccess::private_user};
    auto first_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    auto second_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(first_result && second_result);
    SocketStream nonreading = std::move(first_result.value());
    SocketStream sibling = std::move(second_result.value());
    int send_buffer = 256;
    CHECK(::setsockopt(nonreading.native_handle(), SOL_SOCKET, SO_SNDBUF,
                       &send_buffer, static_cast<socklen_t>(sizeof(send_buffer))) == 0);

    auto attach = [&](SocketStream& stream, const wire::AcquireResponse& lease,
                      wire::FrameReceiver& framer) noexcept {
        std::array<std::uint8_t, kMaxControlPayload> attach_payload{};
        const auto size = wire::encode_attach_request(
        control->attachment(lease.lease_id),
            MutableByteView{attach_payload.data(), attach_payload.size()});
        if (!size || !send_raw_request(
                stream, MessageType::ATTACH_STREAM, 4U,
                ByteView{attach_payload.data(), size.value()})) return false;
        FrameCollector frames;
        return read_raw_frames(stream, framer, frames, 1U) &&
               frames.headers[0U].opcode == wire::kOpcodeAttachStream &&
               frames.headers[0U].kind == wire::Kind::response;
    };
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> first_storage{};
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> second_storage{};
    wire::FrameReceiver first_framer(MutableByteView{first_storage.data(), first_storage.size()});
    wire::FrameReceiver second_framer(
        MutableByteView{second_storage.data(), second_storage.size()});
    CHECK(attach(nonreading, first_lease.value(), first_framer));
    CHECK(attach(sibling, second_lease.value(), second_framer));

    std::atomic<bool> sibling_stop{false};
    std::atomic<bool> sibling_ok{true};
    std::atomic<std::size_t> sibling_data{0U};
    std::mutex sibling_mutex;
    std::condition_variable sibling_started_condition;
    std::condition_variable sibling_progress_condition;
    bool sibling_started = false;
    std::thread sibling_reader([&]() {
        {
            std::lock_guard<std::mutex> lock(sibling_mutex);
            sibling_started = true;
        }
        sibling_started_condition.notify_all();
        std::array<std::uint8_t, 8192U> input{};
        FrameCollector frames;
        while (!sibling_stop.load()) {
            frames.clear();
            const auto read = sibling.read_some(
                MutableByteView{input.data(), input.size()}, Timeout{20U});
            if (!read) {
                if (read.error() == Error::TIMEOUT) continue;
                sibling_ok.store(false);
                sibling_progress_condition.notify_all();
                return;
            }
            if (!second_framer.feed({input.data(), read.value()}, frames)) {
                sibling_ok.store(false);
                sibling_progress_condition.notify_all();
                return;
            }
            for (std::size_t index = 0U; index < frames.count; ++index) {
                if (frames.headers[index].opcode == wire::kOpcodeTsDataEvent) {
                    ++sibling_data;
                    sibling_progress_condition.notify_all();
                }
            }
        }
    });
    {
        std::unique_lock<std::mutex> lock(sibling_mutex);
        CHECK(sibling_started_condition.wait_for(lock, std::chrono::seconds(5),
                                                 [&]() noexcept {
                                                     return sibling_started;
                                                 }));
    }
    CHECK(observation.wait_for_write_blocked());
    {
        std::unique_lock<std::mutex> lock(sibling_mutex);
        (void)sibling_progress_condition.wait_for(
            lock, std::chrono::seconds(5), [&]() noexcept {
                return !sibling_ok.load() || sibling_data.load() != 0U;
            });
    }
    CHECK(sibling_ok.load() && sibling_data.load() != 0U);
    sibling_stop.store(true);
    sibling_reader.join();

    const auto stopped = request(*control, MessageType::STOP_STREAM,
                                 LeaseRequestPayload{second_lease.value().lease_id});
    CHECK(stopped);
    wire::StreamEndEvent end{};
    CHECK(read_until_stream_end(sibling, second_framer, end));
    CHECK(end_error_is(end, ErrorCode::OK) &&
          counters_are(end.final.statistics, 17U, 4U));
    const auto sibling_final = stream_control.final_snapshot(stream_control.final_identity);
    CHECK(sibling_final && sibling_final.value().counters.bytes == 3196U &&
          sibling_final.value().counters.usb_errors == 5U);
    CHECK(request(*control, MessageType::RELEASE,
                  LeaseRequestPayload{second_lease.value().lease_id}));

    // The first peer is intentionally still connected and non-reading.  Stop
    // the poll thread and exercise shutdown directly; it must close/cleanup
    // without waiting for that peer to flush STREAM_END.
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    const auto shut_down = server->shutdown();
    if (!shut_down) std::fprintf(stderr, "nonreading shutdown error=%u\n",
        static_cast<unsigned>(shut_down.error()));
    CHECK(shut_down);
    CHECK(stream_control.detach_calls == 2U);
    server_guard.dismiss();
    return true;
}

}  // namespace

bool run_control_integration_tests()
{
    bool okay = true;
    // Preserve all fourteen cases and their failures while letting independent
    // lanes/data paths report in one bounded run instead of hiding behind the
    // first contract failure.
#define RUN_CONTROL_CASE(name) do { \
    const bool passed = name(); \
    std::fprintf(stderr, "%s %s\n", passed ? "PASS" : "FAIL", #name); \
    okay = passed && okay; \
} while (false)
    RUN_CONTROL_CASE(test_control_server_card_flow_and_shutdown);
    RUN_CONTROL_CASE(test_card_client_ownership_contract);
    RUN_CONTROL_CASE(test_no_hello_timeout_and_malformed_disconnect);
    RUN_CONTROL_CASE(test_control_server_tuner_flow);
    RUN_CONTROL_CASE(test_control_server_lanes_are_independent);
    RUN_CONTROL_CASE(test_control_server_disconnect_during_blocked_acquire);
    RUN_CONTROL_CASE(test_control_server_disconnect_during_blocked_tune);
    RUN_CONTROL_CASE(test_stream_endpoint_attach_and_stream_end);
    RUN_CONTROL_CASE(test_stream_endpoint_terminal_mapping);
    RUN_CONTROL_CASE(test_stream_attach_submit_failure_cleanup);
    RUN_CONTROL_CASE(test_stream_hup_before_attach_completion_cleanup);
    RUN_CONTROL_CASE(test_stream_endpoint_fragmented_and_duplicate);
    RUN_CONTROL_CASE(test_stream_endpoint_capacity_and_retired_reaping);
    RUN_CONTROL_CASE(test_stream_endpoint_nonreading_peer_isolated);
#undef RUN_CONTROL_CASE
    return okay;
}
