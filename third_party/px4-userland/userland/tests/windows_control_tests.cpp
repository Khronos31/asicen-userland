// SPDX-License-Identifier: GPL-2.0-only
// Windows native control-plane round trip: a PosixControlServer with mock
// card/tuner services accepts a typed UDTV client over the Windows AF_UNIX
// adapter and answers STATUS and CARD_* requests, including the shared card
// ownership/transaction contract. This exercises the shared control
// server/worker path plus the Windows socket engine end to end. It requires
// native AF_UNIX (Wine reports WSAEAFNOSUPPORT), so it is native-only.
#include "px4/card_service.h"
#include "asicen/control_context_builder.h"
#include "test_wire_client.h"
#include "px4/control_server.h"
#include "px4/tuner_service.h"
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
namespace wire = dtv::userland::wire;
using asicen::test::WireClient;

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
    std::uint8_t receiver_count() const noexcept override { return 4U; }
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

bool make_context(const TunerService& tuner, const char* instance, ControlContext& context)
{
    const auto* profile = asicen::find_profile(asicen::ModelId::W3u3);
    wire::Epoch epoch{};
    epoch[0] = 1U;
    return profile != nullptr && asicen::build_asicen_mock_control_context(
        *profile, tuner, instance, epoch, context);
}

// The old shared helper speaks the retired product wire and opts into generation
// binding. UDTV always binds handles; keep the ownership and generation scenarios
// here on the actual typed protocol, without reviving a legacy server entrance.
bool card_contract(const EndpointConfig& endpoint, std::atomic<bool>& present)
{
#define CARD_CHECK(condition)                                                        \
    do { if (!(condition)) {                                                         \
        std::fprintf(stderr, "card contract failed at %s:%d: %s\n",                  \
                     __FILE__, __LINE__, #condition);                               \
        ++failures; present.store(true); return false; } } while (false)
    auto first = WireClient::connect(endpoint, wire::kFeatureCardService);
    auto second = WireClient::connect(endpoint, wire::kFeatureCardService);
    auto third = WireClient::connect(endpoint, wire::kFeatureCardService);
    CARD_CHECK(first && second && third);
    const auto connect = [](WireClient& client, wire::Share share) {
        const auto reply = client.request(wire::kOpcodeCardConnect,
            wire::CardConnectRequest{{client.epoch, 0U}, share, 1U},
            wire::encode_card_connect_request);
        if (!reply) return Result<wire::CardSessionResult>::failure(reply.error());
        return wire::decode_card_session_result(
            ByteView{reply.value().payload.data(), reply.value().payload.size()});
    };
    const auto key = [](const wire::CardSessionResult& result) {
        return wire::CardHandleKey{result.epoch, result.reader_id,
                                   result.bound_generation, result.card_handle};
    };
    const auto transmit = [](WireClient& client, const wire::CardHandleKey& handle) {
        wire::CardTransmitRequest value{};
        value.key = handle;
        value.command.length = 2U;
        value.command.bytes[0] = 0x00U;
        value.command.bytes[1] = 0x84U;
        return client.request(wire::kOpcodeCardTransmit, value,
                              wire::encode_card_transmit_request);
    };
    const auto begin = [](WireClient& client, const wire::CardHandleKey& handle) {
        return client.request(wire::kOpcodeBeginTransaction,
            wire::BeginTransactionRequest{handle}, wire::encode_begin_transaction_request);
    };
    const auto end = [](WireClient& client, const wire::CardHandleKey& handle,
                        wire::Disposition disposition) {
        return client.request(wire::kOpcodeEndTransaction,
            wire::EndTransactionRequest{handle, disposition}, wire::encode_end_transaction_request);
    };
    const auto reset = [](WireClient& client, const wire::CardHandleKey& handle) {
        const auto reply = client.request(wire::kOpcodeCardReset,
            wire::CardResetRequest{handle}, wire::encode_card_reset_request);
        if (!reply) return Result<wire::CardSessionResult>::failure(reply.error());
        return wire::decode_card_session_result(
            ByteView{reply.value().payload.data(), reply.value().payload.size()});
    };
    const auto reconnect = [](WireClient& client, const wire::CardHandleKey& handle,
                              wire::Share share) {
        const auto reply = client.request(wire::kOpcodeCardReconnect,
            wire::CardReconnectRequest{handle, share, wire::Disposition::reset, 1U},
            wire::encode_card_reconnect_request);
        if (!reply) return Result<wire::CardSessionResult>::failure(reply.error());
        return wire::decode_card_session_result(
            ByteView{reply.value().payload.data(), reply.value().payload.size()});
    };
    const auto disconnect = [](WireClient& client, const wire::CardHandleKey& handle,
                               wire::Disposition disposition) {
        const auto reply = client.request(wire::kOpcodeCardDisconnect,
            wire::CardDisconnectRequest{handle, disposition}, wire::encode_card_disconnect_request);
        if (!reply) return false;
        const auto decoded = wire::decode_card_disconnect_response(
            ByteView{reply.value().payload.data(), reply.value().payload.size()});
        return decoded && decoded.value().consumed && decoded.value().quiesced &&
            !decoded.value().error.present;
    };
    const auto a = connect(*first, wire::Share::shared);
    const auto b = connect(*second, wire::Share::shared);
    CARD_CHECK(a && b);
    auto akey = key(a.value());
    auto bkey = key(b.value());
    CARD_CHECK(akey.card_handle != 0U && bkey.card_handle != 0U &&
               akey.card_handle != bkey.card_handle);
    const auto denied = connect(*third, wire::Share::exclusive);
    CARD_CHECK(!denied && denied.error() == Error::BUSY);
    const auto foreign = reset(*second, akey);
    CARD_CHECK(!foreign && foreign.error() == Error::NOT_FOUND);
    const auto upgrade_denied = reconnect(*second, bkey, wire::Share::exclusive);
    CARD_CHECK(!upgrade_denied && upgrade_denied.error() == Error::BUSY);
    CARD_CHECK(begin(*first, akey));
    const auto competing = begin(*second, bkey);
    CARD_CHECK(!competing && competing.error() == Error::BUSY);
    const auto blocked = transmit(*second, bkey);
    CARD_CHECK(!blocked && blocked.error() == Error::BUSY);
    const auto reset_owner = reset(*first, akey);
    CARD_CHECK(reset_owner);
    akey = key(reset_owner.value());
    const auto still_busy = transmit(*second, bkey);
    CARD_CHECK(!still_busy && still_busy.error() == Error::BUSY);
    CARD_CHECK(end(*first, akey, wire::Disposition::reset));
    const auto sent = transmit(*second, bkey);
    CARD_CHECK(sent);
    const auto response = wire::decode_card_transmit_response(
        ByteView{sent.value().payload.data(), sent.value().payload.size()});
    CARD_CHECK(response && response.value().response.length == 2U &&
        response.value().response.bytes[0] == 0x6fU && response.value().response.bytes[1] == 0U);
    CARD_CHECK(begin(*first, akey));
    first->close();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        const auto recovered = begin(*second, bkey);
        if (recovered) break;
        CARD_CHECK(recovered.error() == Error::BUSY &&
                   std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CARD_CHECK(end(*second, bkey, wire::Disposition::leave));
    const auto upgraded = reconnect(*second, bkey, wire::Share::exclusive);
    CARD_CHECK(upgraded && upgraded.value().atr.present && upgraded.value().atr.value.length > 0U);
    bkey = key(upgraded.value());
    const auto shared_denied = connect(*third, wire::Share::shared);
    CARD_CHECK(!shared_denied && shared_denied.error() == Error::BUSY);
    CARD_CHECK(disconnect(*second, bkey, wire::Disposition::reset));
    const auto consumed = transmit(*second, bkey);
    CARD_CHECK(!consumed && consumed.error() == Error::NOT_FOUND);
    const auto exclusive = connect(*third, wire::Share::exclusive);
    CARD_CHECK(exclusive);
    CARD_CHECK(disconnect(*third, key(exclusive.value()), wire::Disposition::leave));

    // Both clients are generation-bound. No transparent legacy recovery remains.
    const auto c = connect(*second, wire::Share::shared);
    const auto sibling = connect(*second, wire::Share::shared);
    const auto d = connect(*third, wire::Share::shared);
    CARD_CHECK(c && sibling && d);
    auto ckey = key(c.value());
    auto sibling_key = key(sibling.value());
    auto dkey = key(d.value());
    const auto handle_status = [&](WireClient& client, const wire::CardHandleKey& handle) {
        return client.request(wire::kOpcodeCardHandleStatus,
            wire::CardHandleStatusRequest{handle}, wire::encode_card_handle_status_request);
    };
    const auto initial = handle_status(*second, ckey);
    CARD_CHECK(initial);
    const auto initialized = wire::decode_card_session_result(
        ByteView{initial.value().payload.data(), initial.value().payload.size()});
    CARD_CHECK(initialized && initialized.value().present && initialized.value().initialized &&
               initialized.value().atr.present && initialized.value().atr.value.length > 0U);
    const auto foreign_status = handle_status(*second, dkey);
    CARD_CHECK(!foreign_status && foreign_status.error() == Error::NOT_FOUND);
    const auto observe = [&](bool inserted) {
        present.store(inserted);
        const auto reply = second->request(wire::kOpcodeCardStatus,
            wire::CardStatusRequest{{second->epoch, 0U}}, wire::encode_card_status_request);
        if (!reply) return false;
        const auto decoded = wire::decode_card_status_response(
            ByteView{reply.value().payload.data(), reply.value().payload.size()});
        return decoded && decoded.value().present == inserted;
    };
    CARD_CHECK(begin(*second, ckey));
    CARD_CHECK(observe(false) && observe(true));
    const auto stale = transmit(*second, ckey);
    CARD_CHECK(!stale && stale.error() == Error::CARD_REMOVED);
    const auto stale_status = handle_status(*second, ckey);
    CARD_CHECK(!stale_status && stale_status.error() == Error::CARD_REMOVED);
    const auto stale_begin = begin(*second, sibling_key);
    CARD_CHECK(!stale_begin && stale_begin.error() == Error::CARD_REMOVED);
    const auto rebound = reconnect(*second, ckey, wire::Share::shared);
    CARD_CHECK(rebound && rebound.value().bound_generation != ckey.bound_generation);
    ckey = key(rebound.value());
    CARD_CHECK(transmit(*second, ckey) && handle_status(*second, ckey));
    const auto other_stale = transmit(*third, dkey);
    CARD_CHECK(!other_stale && other_stale.error() == Error::CARD_REMOVED);
    const auto sibling_stale = transmit(*second, sibling_key);
    CARD_CHECK(!sibling_stale && sibling_stale.error() == Error::CARD_REMOVED);
    const auto reset_sibling = reset(*second, sibling_key);
    CARD_CHECK(reset_sibling);
    sibling_key = key(reset_sibling.value());
    CARD_CHECK(transmit(*second, sibling_key));
    const auto reset_other = reset(*third, dkey);
    CARD_CHECK(reset_other);
    dkey = key(reset_other.value());
    CARD_CHECK(transmit(*third, dkey));
    CARD_CHECK(observe(false) && observe(true));
    CARD_CHECK(disconnect(*second, ckey, wire::Disposition::reset));
    CARD_CHECK(disconnect(*second, sibling_key, wire::Disposition::leave));
    CARD_CHECK(disconnect(*third, dkey, wire::Disposition::leave));
    return true;
#undef CARD_CHECK
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
    ControlContext context{};
    if (!make_context(tuner_service, endpoint.instance, context)) {
        CHECK(false);
        return false;
    }
    auto server = PosixControlServer::create(endpoint, card_service, tuner_service, context);
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

    CHECK(card_contract(endpoint, card_backend.present));

    auto client = WireClient::connect(endpoint);
    if (!client) {
        std::fprintf(stderr, "stage typed client connect failed\n");
        CHECK(false);
    } else {
        wire::StatusRequest status_request{};
        status_request.epoch = client->epoch;
        const auto status = client->request(wire::kOpcodeStatus, status_request,
                                             wire::encode_status_request);
        CHECK(status);
        if (status) {
            const auto decoded = wire::decode_status_response(
                ByteView{status.value().payload.data(), status.value().payload.size()});
            CHECK(decoded && decoded.value().context.epoch == client->epoch);
        }
        client->close();
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
        ControlContext rejected_context{};
        CHECK(make_context(tuner_service, rejected.instance, rejected_context));
        CHECK(!PosixControlServer::create(rejected, card_service, tuner_service,
                                         rejected_context));
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
        ControlContext fallback_context{};
        CHECK(make_context(tuner_service, fallback.instance, fallback_context));
        auto default_server = PosixControlServer::create(fallback, card_service,
            tuner_service, fallback_context);
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
