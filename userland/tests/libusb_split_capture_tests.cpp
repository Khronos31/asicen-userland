// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/libusb_hardware_backend.h"


#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <map>
#include <tuple>
#include <thread>
#include <vector>
#include <string>
#include <sstream>

#include "transport_transform_reference.h"
#include <unistd.h>

[[noreturn]] void fixture_abort(int line)
{
    std::printf("Fatal fixture failure at line %d\n", line);
    std::fflush(stdout);
    std::abort();
}

namespace asicen {
struct LibusbW3u3HardwareTestPeer {
    using Hooks = LibusbW3u3Hardware::CaptureUsbHooks;
    static void configure(LibusbW3u3Hardware& hardware, const Hooks* hooks, std::uint8_t receiver)
    {
        if (hardware.profile_ == nullptr) {
            hardware.profile_ = find_profile(ModelId::W3u3);
        }
        hardware.capture_usb_hooks_ = hooks;
        hardware.claimed_ = true;
        hardware.initialized_ = true;
        hardware.tuned_ = true;
        hardware.tuned_receiver_ = receiver;
        hardware.tuned_frequency_khz_ = 557142U;
        hardware.tuned_system_ = receiver == 0U ? px4::userland::ipc::System::ISDB_S
                                                : px4::userland::ipc::System::ISDB_T;
        (void)hardware.active_receiver_.reserve(receiver);
    }

    static bool claim_fixture(LibusbW3u3Hardware& hardware, UsbFunctionClaim& primary,
                              UsbFunctionClaim* sibling)
    {
        const auto* profile = hardware.profile_;
        return profile != nullptr &&
               hardware.ownership_.claim_profile(*profile, primary, sibling, {1U, 2U, 1U},
                                                 sibling == nullptr
                                                     ? std::vector<std::uint8_t>{}
                                                     : std::vector<std::uint8_t>{1U, 2U, 2U}) ==
                   OwnershipError::none;
    }
    static void set_link_seed(LibusbW3u3Hardware& hardware,
                              const std::array<std::uint8_t, 16>& seed)
    {
        hardware.link_seed_ = seed;
    }
};
}
namespace {
using namespace asicen;
using px4::userland::ipc::System;
using Hooks = LibusbW3u3HardwareTestPeer::Hooks;
class FakeFunction final : public UsbFunctionClaim {
public:
    UsbFunctionSnapshot state{};
    UsbFunctionSnapshot snapshot() const override { return state; }
    int claim_interface0() override { return 0; }
    int release_interface0() noexcept override { return 0; }
};

struct FakeUsb {
    LibusbW3u3Hardware* hardware = nullptr;
    FakeFunction primary, sibling;
    std::array<std::uint8_t, 256> controller{};
    std::array<std::array<std::uint8_t, 0x45>, 2> cf{}, saved_cf{};
    std::atomic<int> submit_count{0};
    int fail_submit_number = 0, cancel_count = 0, free_count = 0;
    int seed_clear_count = 0;
    bool seed_clear_values_zero = true, seed_cleanup_active = false;
    const std::array<std::uint8_t, 16>* seed_override = nullptr;
    std::atomic<unsigned> callback_total{0}, pump_count{0};
    std::mutex event_mutex;
    std::condition_variable event_changed;
    bool interrupted_events = false;
    FakeUsb() {
        controller[0x09] = 0x1e;
        for (std::size_t local = 0; local < cf.size(); ++local) {
            for (std::size_t i = 0; i < cf[local].size(); ++i)
                cf[local][i] = static_cast<std::uint8_t>(0x30U + local + i);
            cf[local][0x40] = 4;
        }
    }
    static int control_hook(void* p, const ControlTransfer& t, unsigned char* r) {
        return static_cast<FakeUsb*>(p)->control(t, r);
    }
    static libusb_transfer* allocate_hook(void*) {
        return static_cast<libusb_transfer*>(std::calloc(1, sizeof(libusb_transfer)));
    }
    static int submit_hook(void* p, libusb_transfer*) {
        auto& f = *static_cast<FakeUsb*>(p);
        return ++f.submit_count == f.fail_submit_number ? LIBUSB_ERROR_IO : 0;
    }
    static int cancel_hook(void* p, libusb_transfer* t) {
        auto& f = *static_cast<FakeUsb*>(p);
        ++f.cancel_count;
        t->status = LIBUSB_TRANSFER_CANCELLED;
        t->actual_length = 0;
        t->callback(t);
        ++f.callback_total;
        return 0;
    }
    static void free_hook(void* p, libusb_transfer* t) {
        ++static_cast<FakeUsb*>(p)->free_count;
        std::free(t);
    }
    static int pump_wait_hook(void* context, unsigned timeout_ms)
    {
        auto& fake = *static_cast<FakeUsb*>(context);
        ++fake.pump_count;
        std::unique_lock<std::mutex> lock(fake.event_mutex);
        fake.event_changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                    [&] { return fake.interrupted_events; });
        fake.interrupted_events = false;
        return 0;
    }

    static void interrupt_events_hook(void* context)
    {
        auto& fake = *static_cast<FakeUsb*>(context);
        {
            std::lock_guard<std::mutex> lock(fake.event_mutex);
            fake.interrupted_events = true;
        }
        fake.event_changed.notify_all();
    }

    Hooks hooks()
    {
        return {this,        control_hook, allocate_hook,  submit_hook,
                cancel_hook, free_hook,    pump_wait_hook, interrupt_events_hook};
    }

    int control(const ControlTransfer& transfer, unsigned char* response) {
        const auto status = [&] {
            if (response != nullptr && transfer.length > 0) response[0] = 1;
            return static_cast<int>(transfer.length);
        };
        switch (transfer.request) {
        case Request::SysCtrlRead:
            if (response != nullptr && transfer.length >= 3) {
                response[0] = 1;
                response[1] = 0x11;
                response[2] = 0x52;
            }
            return transfer.length;
        case Request::I2cRead: {
            if (response == nullptr || transfer.length == 0) return LIBUSB_ERROR_IO;
            response[0] = 1;
            const auto slave = static_cast<std::uint8_t>(transfer.value);
            const auto first = static_cast<std::uint8_t>(transfer.value >> 8U);
            for (unsigned i = 1; i < transfer.length; ++i)
                response[i] = slave == 0x4a ? controller[static_cast<std::uint8_t>(first + i - 1)] : 0;
            return transfer.length;
        }
        case Request::I2cWrite:
        case Request::I2cWriteNoStop: {
            const auto slave = static_cast<std::uint8_t>(transfer.value);
            const auto first = static_cast<std::uint8_t>(transfer.value >> 8U);
            if (slave == 0x4a && transfer.length > 1) {
                const auto value = static_cast<std::uint8_t>(transfer.index);
                if (first >= 0x10 && first <= 0x1f && seed_cleanup_active) {
                    ++seed_clear_count;
                    seed_clear_values_zero = seed_clear_values_zero && value == 0;
                }
                controller[first] = value;
                if (transfer.length > 2)
                    controller[static_cast<std::uint8_t>(first + 1)] = static_cast<std::uint8_t>(transfer.index >> 8U);
            }
            return status();
        }
        case Request::ChannelFilterRead: {
            if (response == nullptr || transfer.length == 0U) {
                return LIBUSB_ERROR_IO;
            }
            response[0] = 0U;  // W3U3 CF reads use transfer length, not this byte.
            const std::uint8_t subcmd = static_cast<std::uint8_t>(transfer.value & 0xffU);
            const std::size_t local = (subcmd & 0x80U) != 0U ? 1U : 0U;
            const std::size_t offset = subcmd & 0x7fU;
            const std::size_t count = transfer.length - 1U;
            for (std::size_t i = 0; i < count && offset + i < cf[local].size(); ++i) {
                response[i + 1U] = cf[local][offset + i];
            }
            return static_cast<int>(transfer.length);
        }
        case Request::ChannelFilterWrite: {
            const std::uint8_t subcmd = static_cast<std::uint8_t>(transfer.value & 0xffU);
            const std::size_t local = (subcmd & 0x80U) != 0U ? 1U : 0U;
            const std::size_t offset = subcmd & 0x7fU;
            const std::size_t count = transfer.length - 1U;
            if (offset < cf[local].size()) {
                cf[local][offset] = static_cast<std::uint8_t>(transfer.value >> 8U);
            }
            if (count > 1U && offset + 1U < cf[local].size()) {
                cf[local][offset + 1U] = static_cast<std::uint8_t>(transfer.index & 0xffU);
            }
            if (count > 2U && offset + 2U < cf[local].size()) {
                cf[local][offset + 2U] = static_cast<std::uint8_t>(transfer.index >> 8U);
            }
            return static_cast<int>(transfer.length);
        }
        case Request::DscStart:
            seed_cleanup_active = false;
            if (seed_override) LibusbW3u3HardwareTestPeer::set_link_seed(*hardware, *seed_override);
            return status();
        case Request::DscStop:
            seed_cleanup_active = true;
            return status();
        case Request::ResetChannel: {
            const std::size_t local = static_cast<std::uint8_t>(transfer.value) == 1U ? 1U : 0U;
            cf[local].fill(0U);
            return status();
        }
        default:
            return status();
        }
    }
    std::array<std::uint8_t, 0x45> initial_cf(std::size_t local) const { return saved_cf[local]; }
    void save_initial_cf() { saved_cf = cf; }
};
bool check(bool condition, const char* expression, int line)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "line %d: CHECK failed: %s\n", line, expression);
    return false;
}
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!check((expr), #expr, __LINE__))                                                       \
            return false;                                                                          \
    } while (false)

void configure(LibusbW3u3Hardware& hardware, FakeUsb& fake, Hooks& hooks, std::uint8_t receiver)
{
    hooks = fake.hooks();
    fake.hardware = &hardware;
    fake.save_initial_cf();
    LibusbW3u3HardwareTestPeer::configure(hardware, &hooks, receiver);
    const auto* profile = hardware.device_profile();
    UsbFunctionSnapshot snapshot{};
    snapshot.vendor_id = profile->vid;
    snapshot.product_id = profile->pid;
    snapshot.bus = 1U;
    snapshot.address = 4U;
    snapshot.port_path = {1U, 2U, 1U};
    snapshot.interface0_present = true;
    snapshot.kernel_driver_state_known = true;
    snapshot.active_alt0 = 0;
    snapshot.endpoint81_in_alt0 = snapshot.endpoint81_bulk_in_alt0 = true;
    snapshot.endpoint82_in_alt0 = snapshot.endpoint82_bulk_in_alt0 = true;
    fake.primary.state = snapshot;
    snapshot.address = 5U;
    snapshot.port_path = {1U, 2U, 2U};
    fake.sibling.state = snapshot;
    if (!LibusbW3u3HardwareTestPeer::claim_fixture(hardware, fake.primary,
            profile->expected_runtime_functions == 2U ? &fake.sibling : nullptr)) fixture_abort(__LINE__);
}
} // namespace
namespace {
namespace split_test {
using Bytes = std::vector<std::uint8_t>;
using Seed = std::array<std::uint8_t, 16>;
constexpr std::size_t chunk = 65536, packet = 188;

Bytes packets(std::size_t first, std::size_t count)
{
    Bytes result(count * packet);
    for (std::size_t n = 0; n < count; ++n) {
        auto* p = result.data() + n * packet;
        const auto seq = first + n;
        for (std::size_t i = 4; i < packet; ++i)
            p[i] = static_cast<std::uint8_t>(seq * 31U + i * 17U + (seq >> 3U));
        p[0] = 0x47; p[1] = static_cast<std::uint8_t>((seq >> 8U) & 31U);
        p[2] = static_cast<std::uint8_t>(0x30U + (seq >> 4U));
        p[3] = static_cast<std::uint8_t>(0x10U | (seq & 15U));
    }
    return result;
}
Bytes part(const Bytes& bytes, std::size_t start, std::size_t size)
{ return Bytes(bytes.begin() + start, bytes.begin() + start + size); }
Bytes reference(const Bytes& raw, const Seed& seed)
{
    transport_transform_reference::TransportMaterialV7 material{};
    if (!transport_transform_reference::derive_transport_material_v7(seed.data(), seed.size(), &material))
        fixture_abort(__LINE__);
    Bytes output(raw.size() / packet * packet);
    for (std::size_t offset = 0; offset < output.size(); offset += packet)
        if (!transport_transform_reference::transform_transport_packet_v7(raw.data() + offset,
                packet, material.first_des_key.data(), material.second_des_key.data(),
                material.xor_state.data(), output.data() + offset)) fixture_abort(__LINE__);
    return output;
}
template<class Predicate> bool await(Predicate predicate)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

struct Usb : FakeUsb {
    std::mutex mutex;
    std::map<libusb_transfer*, bool> pending;
    std::vector<libusb_transfer*> attempts;
    std::array<libusb_transfer*, 4> initial{};
    int allocations = 0, free_pending = 0;
    bool poison_on_submit = true;
    bool synchronous_once = false;
    Bytes synchronous_bytes;
    int block_submit_number = 0;
    bool submit_blocked = false, submit_release = false;
    std::condition_variable submit_changed;
    std::atomic<unsigned> interruptions{0};

    static Usb& from(void* p) { return *static_cast<Usb*>(static_cast<FakeUsb*>(p)); }
    static libusb_transfer* allocate(void* p)
    {
        auto& self = from(p);
        std::lock_guard<std::mutex> lock(self.mutex);
        ++self.allocations;
        return FakeUsb::allocate_hook(p);
    }
    void complete_locked(libusb_transfer* transfer, const Bytes& bytes,
                         libusb_transfer_status status, int actual)
    {
        if (bytes.size() > chunk || !pending[transfer]) fixture_abort(__LINE__);
        std::memset(transfer->buffer, 0xc3, chunk);
        if (!bytes.empty()) std::memcpy(transfer->buffer, bytes.data(), bytes.size());
        transfer->status = status;
        transfer->actual_length = actual == -999 ? static_cast<int>(bytes.size()) : actual;
        pending[transfer] = false;
        transfer->callback(transfer);
        ++callback_total;
    }
    void complete(std::size_t slot, const Bytes& bytes,
                  libusb_transfer_status status = LIBUSB_TRANSFER_COMPLETED, int actual = -999)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            complete_locked(initial[slot], bytes, status, actual);
        }
        // Model a callback dispatched by an independent control/event caller.
        FakeUsb::interrupt_events_hook(this);
    }
    static int submit(void* p, libusb_transfer* transfer)
    {
        auto& self = from(p);
        std::unique_lock<std::mutex> lock(self.mutex);
        self.attempts.push_back(transfer);
        const auto rc = FakeUsb::submit_hook(p, transfer);
        self.pending[transfer] = rc == 0;
        const int n = self.submit_count.load();
        if (n <= 4) self.initial[static_cast<std::size_t>(n - 1)] = transfer;
        if (rc == 0 && n > 4) {
            if (self.poison_on_submit) std::memset(transfer->buffer, 0xa6, chunk);
            if (self.synchronous_once) {
                self.synchronous_once = false;
                self.complete_locked(transfer, self.synchronous_bytes, LIBUSB_TRANSFER_COMPLETED, -999);
            }
        }
        if (n == self.block_submit_number) {
            self.submit_blocked = true;
            if (!self.submit_changed.wait_for(lock, std::chrono::seconds(5), [&] { return self.submit_release; }))
                fixture_abort(__LINE__);
        }
        return rc;
    }
    bool is_submit_blocked() { std::lock_guard<std::mutex> lock(mutex); return submit_blocked; }
    void release_submit()
    {
        { std::lock_guard<std::mutex> lock(mutex); submit_release = true; }
        submit_changed.notify_all();
    }
    static void interrupt(void* p)
    {
        ++from(p).interruptions;
        FakeUsb::interrupt_events_hook(p);
    }
    static int cancel(void* p, libusb_transfer* transfer)
    {
        auto& self = from(p);
        std::lock_guard<std::mutex> lock(self.mutex);
        self.pending[transfer] = false;
        return FakeUsb::cancel_hook(p, transfer);
    }
    static void free(void* p, libusb_transfer* transfer)
    {
        auto& self = from(p);
        std::lock_guard<std::mutex> lock(self.mutex);
        if (self.pending[transfer]) ++self.free_pending;
        self.pending.erase(transfer);
        FakeUsb::free_hook(p, transfer);
    }
};

struct Fixture {
    Seed seed{};
    Usb usb;
    Hooks hooks{};
    std::atomic<bool> cancelled{false};
    LibusbW3u3Hardware hardware{nullptr, {}, {}, {}, {}};
    Fixture()
    {
        for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<std::uint8_t>(i * 13U + 7U);
        usb.seed_override = &seed;
        configure(hardware, usb, hooks, 0U);
        hooks.allocate = Usb::allocate; hooks.submit = Usb::submit;
        hooks.cancel = Usb::cancel; hooks.free = Usb::free;
        hooks.interrupt_events = Usb::interrupt;
    }
    bool prepare()
    {
        CHECK(hardware.prepare(0U, System::ISDB_S, cancelled).has_value());
        CHECK(usb.submit_count.load() == 4);
        for (const auto* transfer : usb.initial) {
            CHECK(transfer->length == static_cast<int>(chunk));
            CHECK(transfer->timeout == 0U && transfer->endpoint == 0x81U);
        }
        return true;
    }
    bool clean()
    {
        CHECK(usb.free_count == 4 && usb.allocations == 4);
        CHECK(usb.free_pending == 0 && usb.pending.empty());
        CHECK(usb.seed_clear_count == 16 && usb.seed_clear_values_zero);
        CHECK(usb.cf[0] == usb.initial_cf(0));
        CHECK(hardware.stop().has_value());
        CHECK(hardware.stop().has_value());
        CHECK(usb.free_count == 4);
        return true;
    }
};

struct Sink {
    std::mutex mutex;
    std::condition_variable changed;
    bool block_first = false, blocked = false, release = false, timeout = false;
    bool bad_packet_size = false;
    std::size_t reject_at = 0;
    Bytes output;
    std::thread::id thread_id;
    std::atomic<int>* await_submit_count = nullptr;

    void unblock() { { std::lock_guard<std::mutex> lock(mutex); release = true; } changed.notify_all(); }
    bool is_blocked() { std::lock_guard<std::mutex> lock(mutex); return blocked; }
    std::size_t emitted() { std::lock_guard<std::mutex> lock(mutex); return output.size() / packet; }
    static bool emit(void* opaque, const std::uint8_t* bytes, std::size_t size) noexcept
    {
        auto& self = *static_cast<Sink*>(opaque);
        std::unique_lock<std::mutex> lock(self.mutex);
        const auto n = self.output.size() / packet;
        self.thread_id = std::this_thread::get_id();
        if (size != packet) self.bad_packet_size = true;

        self.output.insert(self.output.end(), bytes, bytes + size);
        // Ensure the injection has actually entered submit #5 before a sink
        // rejection, rather than depending on which thread wins publication.
        if (self.await_submit_count && !await([&] { return self.await_submit_count->load() >= 5; }))
            self.timeout = true;
        if (self.block_first && n == 0) {
            self.blocked = true;
            self.changed.notify_all();
            if (!self.changed.wait_for(lock, std::chrono::seconds(5), [&] { return self.release; }))
                self.timeout = true;
        }
        return n + 1 != self.reject_at && !self.timeout;
    }
};

struct Run {
    Fixture& fixture;
    Sink& sink;
    std::atomic<bool> done{false};
    CaptureRunResult result = CaptureRunResult::fatal_drain;
    std::thread::id producer_id;
    std::thread worker;
    Run(Fixture& f, Sink& s) : fixture(f), sink(s), worker([&] {
        producer_id = std::this_thread::get_id();
        result = fixture.hardware.run(fixture.cancelled, Sink::emit, &sink);
        done.store(true);
    }) {}
    ~Run()
    {
        if (worker.joinable()) {
            fixture.cancelled.store(true);
            fixture.hardware.interrupt();
            fixture.usb.release_submit();
            sink.unblock();
            worker.join();
        }
    }
    bool finish()
    {
        CHECK(await([&] { return done.load(); }));
        worker.join();
        CHECK(!sink.timeout && !sink.bad_packet_size);
        CHECK(sink.output.empty() || sink.thread_id != producer_id);
        return true;
    }
};

// An overflow is observable before releasing the deliberately blocked sink.
// pread never changes stdio's write position; no shared fixture state is raced.
struct StderrCapture {
    FILE* file = std::tmpfile();
    int saved = -1;
    StderrCapture()
    {
        if (file == nullptr) fixture_abort(__LINE__);
        std::fflush(stderr); saved = dup(STDERR_FILENO);
        if (saved < 0 || dup2(fileno(file), STDERR_FILENO) < 0) fixture_abort(__LINE__);
    }
    bool contains(const char* needle)
    {
        char bytes[16384]{};
        const auto n = pread(fileno(file), bytes, sizeof(bytes) - 1, 0);
        return n > 0 && std::strstr(bytes, needle) != nullptr;
    }
    std::string read_all()
    {
        std::fflush(stderr);
        std::string result;
        char bytes[4096];
        for (;;) {
            const auto n = pread(fileno(file), bytes, sizeof(bytes), static_cast<off_t>(result.size()));
            if (n < 0) fixture_abort(__LINE__);
            if (n == 0) return result;
            result.append(bytes, static_cast<std::size_t>(n));
        }
    }
    ~StderrCapture()
    {
        std::fflush(stderr);
        if (dup2(saved, STDERR_FILENO) < 0) fixture_abort(__LINE__);
        close(saved);
        std::rewind(file);
        char bytes[4096];
        for (std::size_t n; (n = std::fread(bytes, 1, sizeof(bytes), file)) != 0;)
            std::fwrite(bytes, 1, n, stderr);
        std::fclose(file);
    }
};
} // namespace split_test
} // namespace
namespace {
namespace request_test {
using namespace split_test;
constexpr std::size_t request = 65536;
struct Fixture : split_test::Fixture {
    Fixture()
    {
        hooks.submit = [](void* p, libusb_transfer* t) {
            if (t->length != static_cast<int>(request)) fixture_abort(__LINE__);
            return Usb::submit(p, t); // Checks every initial and replacement.
        };
    }
    bool prepare()
    {
        CHECK(hardware.prepare(0U, System::ISDB_S, cancelled).has_value());
        CHECK(usb.submit_count.load() == 4);
        for (const auto* t : usb.initial) {
            CHECK(t->length == static_cast<int>(request));
            CHECK(t->timeout == 0U && t->endpoint == 0x81U);
            // Exercise the end of the still-64-KiB USB storage. The fake also
            // poisons all 64 KiB on every replacement. ASan bounds-checks both.
            t->buffer[chunk - 1] = 0xa6;
            CHECK(t->buffer[chunk - 1] == 0xa6);
        }
        return true;
    }
};

Bytes full_stream(std::size_t chunks)
{ return part(packets(0, chunks * request / packet + 1), 0, chunks * request); }

bool short_and_zero_completion()
{
    Fixture f; CHECK(f.prepare());
    const auto raw = part(full_stream(1), 0, request - 1);
    f.usb.complete(0, {}, LIBUSB_TRANSFER_TIMED_OUT);
    f.usb.complete(1, raw);
    Sink sink; sink.reject_at = raw.size() / packet;
    Run run(f, sink); CHECK(run.finish()); CHECK(run.result == CaptureRunResult::cancelled);
    CHECK(sink.output == reference(raw, f.seed));
    CHECK(f.hardware.source_framing_counters().input_bytes_accepted == request - 1);
    CHECK(f.hardware.source_counters().empty_intervals == 1);
    CHECK(f.clean()); return true;
}

bool callback_permutations()
{
    std::array<unsigned, 4> order{{0, 1, 2, 3}};
    do {
        Fixture f; CHECK(f.prepare()); const auto raw = packets(0, 16);
        for (auto n : order) f.usb.complete(n, part(raw, n * 4 * packet, 4 * packet));
        Sink sink; sink.reject_at = 16; Run run(f, sink); CHECK(run.finish());
        CHECK(run.result == CaptureRunResult::cancelled && sink.output == reference(raw, f.seed));
        CHECK(f.clean());
    } while (std::next_permutation(order.begin(), order.end()));
    return true;
}

bool invalid_actual_lengths()
{
    for (const int actual : {-1, -2, static_cast<int>(request + 1), static_cast<int>(2 * chunk)}) {
        Fixture f; CHECK(f.prepare());
        f.usb.complete(0, {}, LIBUSB_TRANSFER_COMPLETED, actual);
        Sink sink; Run run(f, sink); CHECK(run.finish());
        CHECK(run.result == CaptureRunResult::usb_error && sink.output.empty());
        CHECK(f.usb.submit_count.load() == 4);
        CHECK(f.hardware.source_framing_counters().input_bytes_accepted == 0);
        CHECK(f.clean());
    }
    return true;
}

bool launch_cancel_and_submit_failure()
{
    {
        Fixture f; CHECK(f.prepare());
        f.hooks.launch_thread = [](pthread_t*, const pthread_attr_t*, NativeThread::Entry, void*) { return EAGAIN; };
        Sink sink; Run run(f, sink); CHECK(run.finish());
        CHECK(run.result == CaptureRunResult::usb_error && sink.output.empty()); CHECK(f.clean());
    }
    {
        Fixture f; CHECK(f.prepare()); Sink sink; Run run(f, sink);
        CHECK(await([&] { return f.usb.pump_count.load() > 0; }));
        f.cancelled.store(true); f.hardware.interrupt(); CHECK(run.finish());
        CHECK(run.result == CaptureRunResult::cancelled && sink.output.empty()); CHECK(f.clean());
    }
    for (const std::size_t reject : {0U, 1U}) {
        Fixture f; CHECK(f.prepare()); f.usb.fail_submit_number = 5;
        const auto raw = full_stream(1); f.usb.complete(0, raw);
        Sink sink; sink.reject_at = reject; sink.await_submit_count = &f.usb.submit_count;
        Run run(f, sink); CHECK(run.finish()); CHECK(run.result == CaptureRunResult::usb_error);
        CHECK(sink.output == part(reference(raw, f.seed), 0, (reject ? 1 : request / packet) * packet));
        CHECK(f.usb.submit_count.load() == 5); CHECK(f.clean());
    }
    return true;
}

bool cancellation_cannot_hide_callback_error()
{
    for (const int status : {-1, static_cast<int>(LIBUSB_TRANSFER_ERROR), static_cast<int>(LIBUSB_TRANSFER_NO_DEVICE)}) {
        Fixture f; CHECK(f.prepare()); f.usb.block_submit_number = 5;
        f.usb.complete(0, full_stream(1));
        Sink sink; sink.block_first = true; sink.reject_at = 1; Run run(f, sink);
        CHECK(await([&] { return sink.is_blocked() && f.usb.is_submit_blocked(); }));
        if (status < 0) {
            f.usb.complete(1, {}, LIBUSB_TRANSFER_COMPLETED, static_cast<int>(request + 1));
            f.cancelled.store(true);
        } else f.usb.complete(1, {}, static_cast<libusb_transfer_status>(status));
        sink.unblock(); CHECK(await([&] { return f.usb.interruptions.load() != 0; }));
        f.usb.release_submit(); CHECK(run.finish());
        CHECK(run.result == (status == LIBUSB_TRANSFER_NO_DEVICE ? CaptureRunResult::disconnected : CaptureRunResult::usb_error));
        CHECK(f.clean());
    }
    return true;
}

} // namespace request_test
} // namespace
namespace {
namespace ingress_test {
using namespace request_test;
using request_test::Fixture;
constexpr std::size_t capacity = 32;

bool blocked_resume_fifo_and_wrap()
{
    Fixture f; CHECK(f.prepare());
    constexpr std::size_t total = capacity * 3 + 7;
    const auto raw = full_stream(total);
    Sink sink; sink.block_first = true; sink.reject_at = raw.size() / packet;
    f.usb.complete(0, part(raw, 0, request)); Run run(f, sink);
    CHECK(await([&] { return sink.is_blocked() && f.usb.submit_count.load() == 5; }));
    for (std::size_t n = 1; n < capacity; ++n) {
        f.usb.complete(n % 4, part(raw, n * request, request));
        CHECK(await([&] { return f.usb.submit_count.load() == static_cast<int>(5 + n); }));
    }
    CHECK(sink.emitted() == 1 && !run.done.load());
    sink.unblock();
    CHECK(await([&] { return sink.emitted() == capacity * request / packet; }));
    // Reuse every ingress block repeatedly after the full initial backlog.
    for (std::size_t n = capacity; n < total; ++n) {
        f.usb.complete(n % 4, part(raw, n * request, request));
        CHECK(await([&] { return sink.emitted() == (n + 1) * request / packet; }));
        if (n + 1 != total)
            CHECK(await([&] { return f.usb.submit_count.load() == static_cast<int>(5 + n); }));
    }
    CHECK(run.finish()); CHECK(run.result == CaptureRunResult::cancelled);
    CHECK(sink.output == reference(raw, f.seed));
    CHECK(f.hardware.source_framing_counters().input_bytes_accepted == raw.size());
    CHECK(f.clean()); return true;
}

bool thirty_third_completion_overflows()
{
    StderrCapture captured;
    Fixture f; CHECK(f.prepare()); const auto raw = full_stream(capacity + 1);
    Sink sink; sink.block_first = true;
    f.usb.complete(0, part(raw, 0, request)); Run run(f, sink);
    CHECK(await([&] { return sink.is_blocked() && f.usb.submit_count.load() == 5; }));
    for (std::size_t n = 1; n < capacity; ++n) {
        f.usb.complete(n % 4, part(raw, n * request, request));
        CHECK(await([&] { return f.usb.submit_count.load() == static_cast<int>(5 + n); }));
    }
    CHECK(!captured.contains("terminal=ingress_overflow"));
    f.usb.complete(0, part(raw, capacity * request, request));
    CHECK(await([&] { return captured.contains("terminal=ingress_overflow"); }));
    CHECK(f.usb.submit_count.load() == 4 + static_cast<int>(capacity));
    CHECK(!run.done.load()); sink.unblock(); CHECK(run.finish());
    CHECK(run.result == CaptureRunResult::usb_error);
    CHECK(sink.output == part(reference(raw, f.seed), 0, packet));
    CHECK(f.clean()); return true;
}

bool full_backlog_cancel_and_error_races()
{
    for (const int status : {-2, -1, static_cast<int>(LIBUSB_TRANSFER_ERROR),
                             static_cast<int>(LIBUSB_TRANSFER_NO_DEVICE)}) {
        Fixture f; CHECK(f.prepare()); const auto raw = full_stream(capacity);
        f.usb.block_submit_number = 4 + capacity;
        Sink sink; sink.block_first = true; sink.reject_at = 1;
        f.usb.complete(0, part(raw, 0, request)); Run run(f, sink);
        CHECK(await([&] { return sink.is_blocked() && f.usb.submit_count.load() == 5; }));
        for (std::size_t n = 1; n < capacity; ++n) {
            f.usb.complete(n % 4, part(raw, n * request, request));
            CHECK(await([&] { return f.usb.submit_count.load() == static_cast<int>(5 + n); }));
        }
        CHECK(await([&] { return f.usb.is_submit_blocked(); }));
        // Callback failure and cancellation arrive while producer is in submit
        // and consumer is in emit, with all 32 software blocks occupied.
        if (status == -1)
            f.usb.complete(0, {}, LIBUSB_TRANSFER_COMPLETED, static_cast<int>(request + 1));
        else if (status >= 0)
            f.usb.complete(0, {}, static_cast<libusb_transfer_status>(status));
        f.cancelled.store(true); f.hardware.interrupt(); sink.unblock();
        f.usb.release_submit(); CHECK(run.finish());
        CHECK(run.result == (status == -2 ? CaptureRunResult::cancelled :
            status == LIBUSB_TRANSFER_NO_DEVICE ? CaptureRunResult::disconnected : CaptureRunResult::usb_error));
        CHECK(sink.output == part(reference(raw, f.seed), 0, packet));
        CHECK(f.usb.submit_count.load() == 4 + static_cast<int>(capacity));
        CHECK(f.clean());
    }
    return true;
}
} // namespace ingress_test
} // namespace

bool validate_accounting(const std::string& output)
{
    for (const char* disabled : {"asicen-split-copy-audit", "asicen-header-audit",
         "asicen-usb-byte-audit", "asicen-capture-timing", "asicen-usbfs-reap",
         "asicen-satellite-read-poll", "source_sha256=", "copy_sha256=", "raw_audited_bytes="})
        CHECK(output.find(disabled) == std::string::npos);
    std::istringstream lines(output);
    std::string line;
    std::size_t captures = 0, overflows = 0;
    while (std::getline(lines, line)) {
        if (line.find("asicen-split-usb-pump version=1 ") != 0) continue;
        std::map<std::string, std::string> fields;
        std::istringstream tokens(line);
        std::string token;
        while (tokens >> token) {
            const auto equals = token.find('=');
            if (equals != std::string::npos)
                fields[token.substr(0, equals)] = token.substr(equals + 1);
        }
        const auto number = [&](const std::string& key) {
            const auto found = fields.find(key);
            if (found == fields.end()) fixture_abort(__LINE__);
            char* end = nullptr;
            const auto value = std::strtoull(found->second.c_str(), &end, 10);
            if (end == found->second.c_str() || *end != '\0') fixture_abort(__LINE__);
            return value;
        };
        CHECK(fields["accounting"] == "reconciled");
        CHECK(number("high_water") <= 32);
        CHECK(number("pool_blocks") == 32 && number("block_bytes") == 65536);
        for (const std::string unit : {"chunks", "bytes"})
            CHECK(number("copied_" + unit) == number("decoder_input_" + unit) + number("discarded_" + unit));
        if (captures == 0) {
            CHECK(number("high_water") == 32);
            CHECK(number("copied_bytes") == 103 * 65536);
            CHECK(number("decoder_input_bytes") == 103 * 65536);
            CHECK(number("discarded_bytes") == 0);
        }
        if (number("ingress_overflow") == 1) {
            ++overflows;
            CHECK(fields["terminal"] == "ingress_overflow");
            CHECK(number("high_water") == 32 && number("overflow_submission") == 32);
            CHECK(number("copied_chunks") == 32 && number("copied_bytes") == 32 * 65536);
            CHECK(number("decoder_input_chunks") == 1 && number("decoder_input_bytes") == 65536);
            CHECK(number("discarded_chunks") == 31 && number("discarded_bytes") == 31 * 65536);
        }
        if (captures >= 2 && captures < 6) {
            CHECK(number("high_water") == 32 && number("copied_bytes") == 32 * 65536);
            CHECK(number("discarded_bytes") == 31 * 65536);
        }
        ++captures;
    }
    CHECK(captures == 42 && overflows == 1);
    return true;
}

int main()
{
    split_test::StderrCapture captured;
    struct Test { const char* name; bool (*run)(); };
    const Test tests[] = {
        {"blocked_resume_fifo_and_wrap", ingress_test::blocked_resume_fifo_and_wrap},
        {"thirty_third_completion_overflows", ingress_test::thirty_third_completion_overflows},
        {"full_backlog_cancel_and_error_races", ingress_test::full_backlog_cancel_and_error_races},
        {"callback_permutations", request_test::callback_permutations},
        {"short_and_zero_completion", request_test::short_and_zero_completion},
        {"invalid_actual_lengths", request_test::invalid_actual_lengths},
        {"launch_cancel_and_submit_failure", request_test::launch_cancel_and_submit_failure},
        {"cancellation_cannot_hide_callback_error", request_test::cancellation_cannot_hide_callback_error},
    };
    for (const auto& test : tests) {
        std::fprintf(stderr, "[ SPLIT CAPTURE RUN ] %s\n", test.name);
        if (!test.run()) return 1;
        std::fprintf(stderr, "[ SPLIT CAPTURE PASS ] %s\n", test.name);
    }
    if (!validate_accounting(captured.read_all())) return 1;
    std::fprintf(stderr, "[ SPLIT CAPTURE PASS ] 8 groups / 42 captures; accounting reconciled\n");
    return 0;
}
