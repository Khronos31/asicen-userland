// SPDX-License-Identifier: GPL-2.0-or-later
// Offline process-level acceptance for the public ASICEN ctl: a real
// asicenctl executable talks to a real UDTV control server over a private
// AF_UNIX endpoint backed by the existing mock tuner backend and a local mock
// card. Nothing here touches a physical daemon, USB device, or PC/SC reader.
#include "asicen/control_context_builder.h"
#include "asicen/device_profile.h"
#include "asicen/px4_mock_backend.h"
#include "px4/card_service.h"
#include "px4/control_server.h"
#include "px4/posix_ipc.h"
#include "px4/posix_tuner_nonce.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using px4::userland::CardAtr;
using px4::userland::CardProtocolSession;
using px4::userland::CardService;
using px4::userland::CardServiceBackend;
using px4::userland::Error;
using px4::userland::Result;
using px4::userland::ByteView;
using px4::userland::MutableByteView;
namespace wire = dtv::userland::wire;

#ifndef ASICENCTL_EXECUTABLE
#error "ASICENCTL_EXECUTABLE must name the built asicenctl binary"
#endif

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                         #condition);                                           \
            ++failures;                                                         \
            return false;                                                       \
        }                                                                       \
    } while (0)

class TestCardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override
    { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override
    { return Result<bool>::success(present); }

    bool present = true;
};

class TestCardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override
    {
        ready_ = true;
        if (atr_present) {
            atr_.length = 2U;
            atr_.bytes[0U] = 0x3bU;
            atr_.bytes[1U] = 0x00U;
        }
        return Result<void>::success();
    }
    Result<std::size_t> transmit(ByteView, MutableByteView response) noexcept override
    {
        if (!atr_present || !ready_ || response.data == nullptr || response.size < 2U)
            return Result<std::size_t>::failure(Error::UNSUPPORTED);
        if (transmit_fails) return Result<std::size_t>::failure(Error::NO_CARD);
        response.data[0U] = 0x90U;
        response.data[1U] = 0x00U;
        return Result<std::size_t>::success(2U);
    }
    bool initialized() const noexcept override { return ready_; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override { ready_ = false; }

    bool atr_present = true;
    bool transmit_fails = false;

private:
    bool ready_ = false;
    CardAtr atr_{};
};

class MockTime final : public px4::userland::TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
};

std::string temporary_root()
{
    char pattern[] = "/tmp/asicen-public-cli.XXXXXX";
    char* created = ::mkdtemp(pattern);
    if (created == nullptr) return {};
    std::string root(created);
    const std::string runtime = root + "/runtime";
    if (::mkdir(runtime.c_str(), 0700) != 0) return {};
    return root;
}

struct RunResult final {
    int status = -1;
    std::string out;
    std::string err;
};

std::string read_file(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

RunResult run_ctl(const std::string& root, const std::string& instance,
                  std::initializer_list<std::string> extra)
{
    RunResult result;
    const std::string out_path = root + "/cli.out";
    const std::string err_path = root + "/cli.err";
    const std::string runtime = root + "/runtime";
    const pid_t child = ::fork();
    if (child == 0) {
        const int out = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int err = ::open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out >= 0) { (void)::dup2(out, STDOUT_FILENO); ::close(out); }
        if (err >= 0) { (void)::dup2(err, STDERR_FILENO); ::close(err); }
        std::vector<std::string> owned{ASICENCTL_EXECUTABLE,
                                        "--runtime-dir", runtime,
                                        "--instance", instance};
        for (const std::string& value : extra) owned.push_back(value);
        std::vector<char*> argv;
        argv.reserve(owned.size() + 1U);
        for (std::string& value : owned) argv.push_back(value.data());
        argv.push_back(nullptr);
        ::execv(ASICENCTL_EXECUTABLE, argv.data());
        std::_Exit(127);
    }
    if (child <= 0) return result;
    int status = 0;
    bool finished = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t received = ::waitpid(child, &status, WNOHANG);
        if (received == child) { finished = true; break; }
        if (received < 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!finished) {
        (void)::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
        result.status = -1;
    } else {
        result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    result.out = read_file(out_path);
    result.err = read_file(err_path);
    return result;
}

struct Server final {
    explicit Server(bool card_present, bool atr_present, bool transmit_fails,
                    bool no_reader)
        : root(temporary_root()), instance("MixedCase"),
          backend{}, session{}, card(backend, session),
          tuner_backend(2U), nonce{}, time{}, tuner(tuner_backend, nonce, time,
                                                    nullptr, nullptr, &stream),
          context{}
    {
        backend.present = card_present;
        session.atr_present = atr_present;
        session.transmit_fails = transmit_fails;
        no_reader_value = no_reader;
    }

    bool start()
    {
        const asicen::DeviceProfile* profile = asicen::find_profile(asicen::ModelId::W3u3);
        if (root.empty() || profile == nullptr) return false;
        const auto epoch = nonce.generate();
        if (!epoch) return false;
        if (!asicen::build_asicen_mock_control_context(
                *profile, tuner, instance.c_str(), epoch.value(), context))
            return false;
        if (no_reader_value) {
            context.list.readers.present = true;
            context.list.readers.value.count = 0U;
        }
        const std::string runtime_path = root + "/runtime";
        const px4::userland::ipc::posix::EndpointConfig endpoint{
            runtime_path.c_str(), instance.c_str(),
            px4::userland::ipc::posix::kControlEndpointName,
            px4::userland::ipc::posix::EndpointAccess::private_user};
        auto created = px4::userland::ipc::posix::PosixControlServer::create(
            endpoint, card, tuner, context, &stream);
        if (!created) return false;
        server = std::move(created.value());
        thread = std::thread([this]() {
            while (!stop.load()) {
                if (!server->poll_once(px4::userland::Timeout{10U})) return;
            }
        });
        return true;
    }

    ~Server()
    {
        stop.store(true);
        if (thread.joinable()) thread.join();
        if (server) (void)server->shutdown();
        (void)card.shutdown();
    }

    std::string root;
    std::string instance;
    bool no_reader_value = false;
    TestCardBackend backend;
    TestCardSession session;
    CardService card;
    asicen::MockTunerBackend tuner_backend;
    asicen::MockTunerStream stream;
    px4::userland::ipc::posix::PosixTunerNonceSource nonce;
    MockTime time;
    px4::userland::TunerService tuner;
    px4::userland::ipc::posix::ControlContext context;
    std::unique_ptr<px4::userland::ipc::posix::PosixControlServer> server;
    std::atomic<bool> stop{false};
    std::thread thread;
};

bool test_list_and_status()
{
    Server server(true, true, false, false);
    CHECK(server.start());

    const RunResult list = run_ctl(server.root, server.instance, {"list"});
    CHECK(list.status == 0);
    CHECK(list.out.rfind("serial=unknown", 0U) == 0U);
    CHECK(list.out.find("ready=ready") != std::string::npos);
    CHECK(list.out.find("receiver=0") != std::string::npos);
    CHECK(list.out.find("system=ISDB-S") != std::string::npos);
    CHECK(list.out.find("system=ISDB-T") != std::string::npos);

    const RunResult status = run_ctl(server.root, server.instance, {"status"});
    CHECK(status.status == 0);
    CHECK(status.out.find("\"name\":\"userland.status\"") != std::string::npos);
    CHECK(status.out.find("\"product\":\"asicen-userland\"") != std::string::npos);
    CHECK(status.out.find("\"MixedCase\"") != std::string::npos);
    return true;
}

bool test_serial_selector_rejected()
{
    const std::string root = temporary_root();
    CHECK(!root.empty());
    const RunResult result = run_ctl(root, "MixedCase",
                                     {"--device", "serial:12345678901234", "list"});
    CHECK(result.status == 2);
    CHECK(result.err.find("invalid --device selector") != std::string::npos);
    return true;
}

bool test_card_status_present_and_absent()
{
    {
        Server present(true, true, false, false);
        CHECK(present.start());
        const RunResult card = run_ctl(present.root, present.instance, {"card-status"});
        CHECK(card.status == 0);
        CHECK(card.out.find("present=yes") != std::string::npos);
        CHECK(card.out.find("initialized=no") != std::string::npos);
    }
    {
        Server absent(false, false, false, false);
        CHECK(absent.start());
        const RunResult card = run_ctl(absent.root, absent.instance, {"card-status"});
        // A missing card is a successful status query, not a connect error.
        CHECK(card.status == 0);
        CHECK(card.out.find("present=no") != std::string::npos);
        CHECK(card.out.find("initialized=no") != std::string::npos);
    }
    return true;
}

bool test_card_status_no_reader()
{
    Server server(true, true, false, true);
    CHECK(server.start());
    const RunResult card = run_ctl(server.root, server.instance, {"card-status"});
    // The daemon has no reader: distinct from an absent card.
    CHECK(card.status == 3);
    CHECK(card.err.find("no card reader") != std::string::npos);
    return true;
}

bool test_card_session_operations()
{
    Server server(true, true, false, false);
    CHECK(server.start());

    const RunResult atr = run_ctl(server.root, server.instance, {"card-atr"});
    CHECK(atr.status == 0);
    CHECK(atr.out.find("atr=3b:00") != std::string::npos);

    const RunResult reset = run_ctl(server.root, server.instance, {"card-reset"});
    CHECK(reset.status == 0);
    CHECK(reset.out.find("atr=3b:00") != std::string::npos);

    const RunResult apdu = run_ctl(server.root, server.instance,
                                   {"card-apdu", "00:a4:00:00"});
    CHECK(apdu.status == 0);
    CHECK(apdu.out.find("response=90:00") != std::string::npos);
    CHECK(apdu.out.find("response-length=2") != std::string::npos);
    CHECK(apdu.out.find("transmit-count=1") != std::string::npos);
    return true;
}

bool test_apdu_boundaries()
{
    Server server(true, true, false, false);
    CHECK(server.start());

    std::string max_hex;
    max_hex.reserve(4096U * 3U);
    for (std::size_t index = 0U; index < 4096U; ++index) {
        if (index != 0U) max_hex.push_back(':');
        max_hex += "00";
    }
    const RunResult accepted = run_ctl(server.root, server.instance,
                                       {"card-apdu", max_hex});
    CHECK(accepted.status == 0);
    CHECK(accepted.out.find("response=90:00") != std::string::npos);

    const std::string too_long = max_hex + ":00";
    const RunResult rejected = run_ctl(server.root, server.instance,
                                       {"card-apdu", too_long});
    CHECK(rejected.status == 2);

    for (const char* malformed : {"", "0", "00:", ":00", "0g", "00:0"}) {
        const RunResult invalid = run_ctl(server.root, server.instance,
                                          {"card-apdu", malformed});
        CHECK(invalid.status == 2);
    }
    return true;
}

bool test_card_failures()
{
    {
        Server server(true, true, false, false);
        CHECK(server.start());
        const RunResult invalid = run_ctl(server.root, server.instance,
                                          {"card-apdu", "not-hex"});
        CHECK(invalid.status == 2);
    }
    {
        Server server(true, true, true, false);
        CHECK(server.start());
        const RunResult failed = run_ctl(server.root, server.instance,
                                         {"card-apdu", "00:a4:00:00"});
        CHECK(failed.status == 9);
        CHECK(failed.err.find("card operation failed") != std::string::npos);
    }
    {
        Server server(false, false, false, false);
        CHECK(server.start());
        const RunResult atr = run_ctl(server.root, server.instance, {"card-atr"});
        CHECK(atr.status != 0);
    }
    return true;
}

}  // namespace

int main()
{
    bool ok = true;
    ok = test_list_and_status() && ok;
    ok = test_serial_selector_rejected() && ok;
    ok = test_card_status_present_and_absent() && ok;
    ok = test_card_status_no_reader() && ok;
    ok = test_card_session_operations() && ok;
    ok = test_apdu_boundaries() && ok;
    ok = test_card_failures() && ok;
    if (!ok || failures != 0) {
        std::fprintf(stderr, "asicen public CLI tests failed (%d)\n", failures);
        return 1;
    }
    std::printf("PASS asicen_public_cli_tests\n");
    return 0;
}
