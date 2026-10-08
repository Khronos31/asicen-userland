// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/product_profile.h"
#include "asicen/px4_mock_backend.h"
#include "px4/control_server.h"
#include "px4/posix_tuner_nonce.h"

#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

int run_asicend_research(int argc, char** argv);

namespace {

volatile sig_atomic_t stop_requested = 0;

void signal_handler(int) { stop_requested = 1; }

class MockTime final : public px4::userland::TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t ms) noexcept override
    { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
};

bool valid_instance(const std::string& value)
{
    if (value.empty() || value.size() > 80U || value == "." || value == "..") return false;
    const bool serial_shaped = (value.size() == 14U || value.size() == 15U) &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        });
    if (serial_shaped) return false;
    for (const unsigned char c : value) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.') continue;
        return false;
    }
    return true;
}

int acquire_enclosure_lock()
{
    const int fd = ::open("/tmp/asicen-userland-enclosure.lock",
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void print_usage(FILE* output)
{
    std::fprintf(output,
        "usage: asicend --mock [--runtime-dir PATH] [--instance TOKEN]\n"
        "       asicend --list | --list-json\n"
        "ASICEN hardware access is disabled in this mock-only build.\n");
}

int list_devices(bool json)
{
    if (json) std::puts("{\"devices\":[],\"serial\":null,\"backend\":\"mock-only\"}");
    else std::puts("No ASICEN hardware backend is enabled (mock-only build).");
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    bool has_mock = false;
    bool has_research_socket = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mock") == 0) has_mock = true;
        if (std::strcmp(argv[i], "--socket") == 0) has_research_socket = true;
    }
    if (has_mock && has_research_socket) return run_asicend_research(argc, argv);
    bool mock = false;
    bool list = false;
    bool list_json = false;
    std::string runtime_directory;
    std::string instance = "default";
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") { print_usage(stdout); return 0; }
        if (arg == "--mock") { mock = true; continue; }
        if (arg == "--list") { list = true; continue; }
        if (arg == "--list-json") { list_json = true; continue; }
        if ((arg == "--runtime-dir" || arg == "--instance") && i + 1 < argc) {
            const std::string value(argv[++i]);
            if (arg == "--runtime-dir") runtime_directory = value;
            else instance = value;
            continue;
        }
        if (arg == "--usb-path" && i + 1 < argc) {
            std::fprintf(stderr, "USB topology selection requires the disabled hardware backend\n");
            return 3;
        }
        print_usage(stderr);
        return 2;
    }
    if (list || list_json) return list_devices(list_json);
    if (!mock) {
        std::fprintf(stderr, "hardware backend disabled; pass --mock for the isolated mock service\n");
        return 3;
    }
    if (!valid_instance(instance) || runtime_directory.size() >= 400U) {
        std::fprintf(stderr, "invalid runtime directory or instance token\n");
        return 2;
    }
    const int enclosure_lock = acquire_enclosure_lock();
    if (enclosure_lock < 0) {
        std::fprintf(stderr, "ASICEN enclosure is already owned or lock path is unsafe\n");
        return 4;
    }

    asicen::MockTunerBackend tuner_backend;
    asicen::MockTunerStream stream;
    asicen::UnsupportedCardBackend card_backend;
    asicen::UnsupportedCardSession card_session;
    px4::userland::CardService card_service(card_backend, card_session);
    px4::userland::ipc::posix::PosixTunerNonceSource nonce_source;
    MockTime time;
    px4::userland::TunerService tuner_service(
        tuner_backend, nonce_source, time, nullptr, nullptr, &stream);
    const px4::userland::ipc::posix::EndpointConfig endpoint{
        runtime_directory.empty() ? nullptr : runtime_directory.c_str(),
        instance.c_str(), px4::userland::ipc::posix::kControlEndpointName};
    auto server = px4::userland::ipc::posix::PosixControlServer::create(
        endpoint, card_service, tuner_service, {}, true,
        asicen::profile::kUsbPresentMask, &stream,
        asicen::profile::kReceiverCount, false);
    if (!server) {
        std::fprintf(stderr, "asicend: %s\n", px4::userland::error_string(server.error()));
        ::close(enclosure_lock);
        return server.error() == px4::userland::Error::BUSY ? 4 : 70;
    }

    struct sigaction action{};
    action.sa_handler = signal_handler;
    ::sigemptyset(&action.sa_mask);
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    ::signal(SIGPIPE, SIG_IGN);
    std::fprintf(stderr, "asicend ready backend=mock-only serial=none receivers=4 endpoint=%s\n",
                 server.value()->endpoint_path());
    px4::userland::Error loop_error = px4::userland::Error::OK;
    while (!stop_requested) {
        const auto polled = server.value()->poll_once(px4::userland::Timeout{100U});
        if (!polled) { loop_error = polled.error(); break; }
    }
    const auto stopped = server.value()->shutdown();
    if (!stopped && loop_error == px4::userland::Error::OK) loop_error = stopped.error();
    server.value().reset();
    ::close(enclosure_lock);
    if (loop_error != px4::userland::Error::OK) {
        std::fprintf(stderr, "asicend shutdown: %s\n", px4::userland::error_string(loop_error));
        return 70;
    }
    return 0;
}
