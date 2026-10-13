// SPDX-License-Identifier: GPL-2.0-only
// Windows native control-plane round trip: a PosixControlServer with mock
// card/tuner services accepts a PosixControlClient over the Windows AF_UNIX
// adapter and answers STATUS and CARD_* requests, including the shared card
// ownership/transaction contract. This exercises the shared control
// server/worker path plus the Windows socket engine end to end. It requires
// native AF_UNIX (Wine reports WSAEAFNOSUPPORT), so it is native-only.
#include "px4/card_service.h"
#include "px4/control_client.h"
#include "px4/control_server.h"
#include "px4/tuner_service.h"
#include "card_client_contract_tests.h"
#include "control_workers.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "windows/windows_security.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

int failures = 0;

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                         #condition);                                               \
            ++failures;                                                             \
        }                                                                           \
    } while (false)

class CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override { return Result<bool>::success(present.load()); }
    std::atomic<bool> present{true};
};

class CardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override
    {
        // A successfully initialized session must expose an ATR, including
        // through the shared CARD_HANDLE_STATUS response codec.
        atr_.bytes[0] = 0x3bU;
        atr_.bytes[1] = 0x80U;
        atr_.bytes[2] = 0x01U;
        atr_.bytes[3] = 0x81U;
        atr_.length = 4U;
        initialized_ = true;
        return Result<void>::success();
    }
    Result<std::size_t> transmit(ByteView apdu, MutableByteView output) noexcept override
    {
        if (output.size < 2U || apdu.size == 0U) {
            return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
        }
        output.data[0] = 0x6fU;
        output.data[1] = 0x00U;
        return Result<std::size_t>::success(2U);
    }
    bool initialized() const noexcept override { return initialized_; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override { initialized_ = false; }

private:
    bool initialized_ = false;
    CardAtr atr_{};
};

template <typename Payload>
Result<ControlResponse> request(PosixControlClient& client, MessageType type,
                                const Payload& payload) noexcept
{
    std::array<std::uint8_t, kMaxControlPayload> encoded{};
    const auto size =
        encode_payload(payload, MutableByteView{encoded.data(), encoded.size()});
    if (!size) {
        return Result<ControlResponse>::failure(size.error());
    }
    return client.request(type, ByteView{encoded.data(), size.value()}, Timeout{4000U});
}

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, kNonceLength>> generate() noexcept override
    {
        return Result<std::array<std::uint8_t, kNonceLength>>::success({});
    }
};

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

class TunerBackend final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<bool> is_locked(std::uint8_t, System) noexcept override
    {
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
    Result<void> close_receiver(std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }
};

std::string unique_runtime_dir(bool unicode = false)
{
    std::array<wchar_t, MAX_PATH> base{};
    const DWORD base_length =
        ::GetEnvironmentVariableW(L"USERPROFILE", base.data(),
                                  static_cast<DWORD>(base.size()));
    if (base_length == 0U || base_length >= base.size()) {
        return {};
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::array<std::uint8_t, 6U> random{};
        std::array<wchar_t, 20U> token{};
        if (!windows_security::random_hex(random.data(), random.size(), token.data(),
                                          token.size())) {
            return {};
        }
        const std::filesystem::path path =
            std::filesystem::path(base.data()) /
            (std::wstring(unicode ? L"px4t-\u65e5-" : L"px4t-") + token.data());
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            continue;
        }
        if (!std::filesystem::create_directories(path, error) || error) {
            return {};
        }
        if (!windows_security::set_current_user_owner_and_dacl(path.c_str())) {
            return {};
        }
        return path.u8string();
    }
    return {};
}

bool test_control_round_trip(const std::string& runtime)
{
    if (runtime.empty()) {
        std::fprintf(stderr, "stage fixture: runtime dir unavailable\n");
        CHECK(false);
        return false;
    }
    CardBackend card_backend;
    CardSession card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    Nonce nonce;
    Time time;
    TunerService tuner_service(tuner_backend, nonce, time);

    const EndpointConfig endpoint{runtime.c_str(), "control",
                                  kControlEndpointName,
                                  EndpointAccess::private_user};
    auto server = PosixControlServer::create(
        endpoint, card_service, tuner_service, "000000000012345", true, 0x03U,
        nullptr, kQ3U4ReceiverCount, false);
    if (!server) {
        std::fprintf(stderr, "stage server create error=%d path=%s\n",
                     static_cast<int>(server.error()), runtime.c_str());
        CHECK(false);
        return false;
    }

    std::atomic<bool> stop{false};
    std::thread poller([&server, &stop]() noexcept {
        while (!stop.load(std::memory_order_acquire)) {
            const auto polled = server.value()->poll_once(Timeout{50U});
            if (!polled) {
                break;
            }
        }
    });

    CHECK(test::run_card_client_contract(endpoint));
    CHECK(test::run_card_generation_contract(endpoint, card_backend.present));

    auto client = PosixControlClient::connect(endpoint, kCapabilityCard, Timeout{4000U});
    if (!client) {
        std::fprintf(stderr, "stage client connect error=%d\n",
                     static_cast<int>(client.error()));
        CHECK(false);
    } else {
        const auto status = client.value()->request(
            MessageType::STATUS, ByteView{nullptr, 0U}, Timeout{4000U});
        CHECK(status);

        const auto connected = request(*client.value(), MessageType::CARD_CONNECT,
                                       CardConnectRequestPayload{ShareMode::shared});
        if (!connected) {
            std::fprintf(stderr, "stage card connect error=%d\n",
                         static_cast<int>(connected.error()));
        }
        CHECK(connected);
        if (connected) {
            const auto card = decode_card_connect_response_payload(
                ByteView{connected.value().payload.data(),
                         connected.value().payload.size()});
            CHECK(card);
            if (card) {
                const std::array<std::uint8_t, 2U> apdu{0x00U, 0x84U};
                const auto transmitted = request(
                    *client.value(), MessageType::CARD_TRANSMIT,
                    CardTransmitRequestPayload{card.value().card_handle,
                                               ByteView{apdu.data(), apdu.size()}});
                if (!transmitted) {
                    std::fprintf(stderr, "stage card transmit error=%d\n",
                                 static_cast<int>(transmitted.error()));
                }
                CHECK(transmitted);
                if (transmitted) {
                    const auto body = decode_card_transmit_response_payload(
                        ByteView{transmitted.value().payload.data(),
                                 transmitted.value().payload.size()});
                    CHECK(body && body.value().response.size == 2U);
                }
            }
        }
    }

    stop.store(true, std::memory_order_release);
    poller.join();
    const auto shutdown = server.value()->shutdown();
    CHECK(shutdown);
    return true;
}

// Test-only environment changes are made before worker threads are started,
// then restored after every worker/server has been destroyed.
class ScopedLocalAppData final {
public:
    ScopedLocalAppData()
    {
        ::SetLastError(ERROR_SUCCESS);
        const DWORD length = ::GetEnvironmentVariableW(
            L"LOCALAPPDATA", saved_.data(), static_cast<DWORD>(saved_.size()));
        present_ = length != 0U || ::GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        valid_ = length < saved_.size();
    }
    ~ScopedLocalAppData()
    {
        if (valid_) CHECK(::SetEnvironmentVariableW(
            L"LOCALAPPDATA", present_ ? saved_.data() : nullptr) != 0);
    }
    bool valid() const noexcept { return valid_; }
    bool set(const std::filesystem::path& path) noexcept
    {
        return valid_ && ::SetEnvironmentVariableW(L"LOCALAPPDATA", path.c_str()) != 0;
    }
    bool matches(const std::filesystem::path& path) const noexcept
    {
        std::array<wchar_t, 32768U> value{};
        const DWORD length = ::GetEnvironmentVariableW(
            L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
        return length < value.size() && path == std::filesystem::path(value.data());
    }
private:
    std::array<wchar_t, 32768U> saved_{};
    bool present_ = false;
    bool valid_ = false;
};

bool add_untrusted_capability_writer(const std::filesystem::path& path)
{
    SECURITY_ATTRIBUTES attributes{};
    SECURITY_DESCRIPTOR descriptor{};
    PACL owner_acl = nullptr;
    windows_security::SidBuffer owner{};
    if (!windows_security::build_current_user_security(
            attributes, descriptor, owner_acl, owner)) return false;
    windows_security::SidBuffer capability{};
    DWORD size = sizeof(capability.bytes);
    const BOOL made = ::CreateWellKnownSid(
        WinCapabilityInternetClientSid, nullptr, capability.bytes, &size);
    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = FILE_WRITE_DATA;
    access.grfAccessMode = GRANT_ACCESS;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(capability.bytes);
    PACL combined = nullptr;
    const bool ok = made != 0 &&
        ::SetEntriesInAclW(1U, &access, owner_acl, &combined) == ERROR_SUCCESS &&
        ::SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, combined, nullptr) == ERROR_SUCCESS;
    if (combined != nullptr) ::LocalFree(combined);
    ::LocalFree(owner_acl);
    return ok;
}

void test_runtime_directory_selection()
{
    const std::string runtime = unique_runtime_dir(true);
    const std::string rejected_runtime = unique_runtime_dir();
    CHECK(!runtime.empty() && !rejected_runtime.empty());
    if (runtime.empty() || rejected_runtime.empty()) return;
    const auto good = std::filesystem::u8path(runtime);
    const auto bad = std::filesystem::u8path(rejected_runtime);
    CHECK(add_untrusted_capability_writer(bad));
    CHECK(windows_security::verify_runtime_parent(good.c_str()));
    CHECK(!windows_security::verify_runtime_parent(bad.c_str()));
    {
        ScopedLocalAppData environment;
        CHECK(environment.valid());
        CHECK(environment.set(bad));
        // The complete server/worker IPC path must honor the explicit UTF-8
        // runtime, even when ambient LOCALAPPDATA fails trusted-writer checks.
        CHECK(test_control_round_trip(runtime));
        CHECK(environment.matches(bad));

        CardBackend card_backend;
        CardSession card_session;
        CardService card_service(card_backend, card_session);
        TunerBackend tuner_backend;
        Nonce nonce;
        Time time;
        TunerService tuner_service(tuner_backend, nonce, time);
        auto explicit_good = ControlWorkerLanes::create(
            card_service, tuner_service, runtime.c_str());
        CHECK(explicit_good);
        if (explicit_good) explicit_good.value().reset();
        const auto default_bad = ControlWorkerLanes::create(card_service, tuner_service);
        CHECK(!default_bad);
        CHECK(environment.matches(bad));

        CHECK(environment.set(good));
        const auto explicit_bad = ControlWorkerLanes::create(
            card_service, tuner_service, rejected_runtime.c_str());
        CHECK(!explicit_bad);
        const EndpointConfig rejected{rejected_runtime.c_str(), "rejected",
            kControlEndpointName, EndpointAccess::private_user};
        CHECK(!PosixControlServer::create(rejected, card_service, tuner_service,
            "000000000012345", true, 0x03U, nullptr, kQ3U4ReceiverCount, false));
        // No fallback for explicit empty/relative/malformed/truncated values.
        const std::array<std::string, 6U> invalid{
            "", ".", "C:relative", std::string("C:\\\xc0\xaf"),
            std::string(MAX_PATH * 4U, 'x'), runtime + std::string(MAX_PATH, 'x')};
        for (const auto& path : invalid) {
            CHECK(!ControlWorkerLanes::create(card_service, tuner_service, path.c_str()));
        }
        auto default_good = ControlWorkerLanes::create(card_service, tuner_service);
        CHECK(default_good);
        if (default_good) default_good.value().reset();
        const EndpointConfig fallback{nullptr, "fallback", kControlEndpointName,
            EndpointAccess::private_user};
        auto default_server = PosixControlServer::create(fallback, card_service,
            tuner_service, "000000000012345", true, 0x03U, nullptr,
            kQ3U4ReceiverCount, false);
        CHECK(default_server);
        if (default_server) default_server.value().reset();
        CHECK(environment.matches(good));
    }
    CHECK(windows_security::set_current_user_owner_and_dacl(bad.c_str()));
    // Never hide a leaked endpoint or wake directory with recursive cleanup.
    std::error_code error;
    CHECK(std::filesystem::remove(good, error) && !error);
    error.clear();
    CHECK(std::filesystem::remove(bad, error) && !error);
}

}  // namespace

int main()
{
    test_runtime_directory_selection();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("windows control tests: PASS\n");
    return 0;
}
