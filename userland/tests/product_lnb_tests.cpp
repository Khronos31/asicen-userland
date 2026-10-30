// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/device_profile.h"
#include "asicen/px4_mock_backend.h"
#include "px4/ipc.h"

#include <array>
#include <charconv>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace px4::userland;
using namespace px4::userland::ipc;

bool check(bool ok, const char* message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!check(__VA_ARGS__)) {                                                                 \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, kNonceLength> result{};
        result[0] = ++sequence_;
        return Result<std::array<std::uint8_t, kNonceLength>>::success(result);
    }

private:
    std::uint8_t sequence_ = 0U;
};

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return now_; }
    void sleep_ms(std::uint32_t milliseconds) noexcept override { now_ += milliseconds; }

private:
    std::uint64_t now_ = 1U;
};

std::uint8_t voltage(const asicen::MockTunerBackend& backend, std::uint8_t receiver)
{
    const auto result = backend.simulated_lnb_voltage(receiver);
    if (!check(result.has_value(), "mock state inspection uses a present receiver")) {
        return 0xffU;
    }
    return result.value();
}

bool mock_transactions_and_model_boundaries()
{
    for (std::size_t model = 0U; model < asicen::profile_count(); ++model) {
        const auto& profile = asicen::profiles()[model];
        asicen::MockTunerBackend backend(profile);
        for (std::uint8_t receiver = 0U; receiver < backend.receiver_count(); ++receiver) {
            CHECK(voltage(backend, receiver) == 0U, "mock starts OFF");
            if (!backend.receiver_supports(receiver, System::ISDB_S)) {
                CHECK(backend.begin_tune_power(receiver, System::ISDB_S, 15U).error() ==
                          Error::INVALID_ARGUMENT,
                      "terrestrial receiver cannot accept satellite ON");
                CHECK(backend.begin_tune_power(receiver, System::ISDB_T, 15U).error() ==
                          Error::INVALID_ARGUMENT,
                      "terrestrial tune cannot request ON");
                continue;
            }
            CHECK(backend.begin_tune_power(receiver, System::ISDB_S, 13U).error() ==
                      Error::INVALID_ARGUMENT,
                  "mock rejects unsupported voltage without a pending transaction");
            CHECK(backend.begin_tune_power(receiver, System::ISDB_S, 15U).has_value() &&
                      voltage(backend, receiver) == 15U,
                  "mock ON precedes acquisition");
            CHECK(backend.begin_tune_power(receiver, System::ISDB_S, 0U).error() == Error::BUSY,
                  "overlapping tune power transaction is rejected");
            CHECK(backend.rollback_tune_power(receiver).has_value() &&
                      voltage(backend, receiver) == 0U,
                  "failed first ON rolls back to OFF");
            CHECK(backend.begin_tune_power(receiver, System::ISDB_S, 15U).has_value() &&
                      backend.commit_tune_power(receiver).has_value(),
                  "successful ON commits");
            CHECK(backend.begin_tune_power(receiver, System::ISDB_S, 0U).has_value() &&
                      voltage(backend, receiver) == 0U,
                  "OFF retune applies the pending request before acquisition");
            CHECK(backend.rollback_tune_power(receiver).has_value() &&
                      voltage(backend, receiver) == 15U,
                  "failed OFF retune restores prior committed ON");
            CHECK(backend.begin_tune_power(receiver, System::ISDB_S, 0U).has_value() &&
                      backend.commit_tune_power(receiver).has_value() &&
                      voltage(backend, receiver) == 0U,
                  "successful OFF commits");
            CHECK(backend.begin_tune_power(receiver, System::ISDB_S, 15U).has_value() &&
                      backend.close_receiver(receiver).has_value() &&
                      voltage(backend, receiver) == 0U,
                  "close clears pending ON and state");
            CHECK(backend.rollback_tune_power(receiver).has_value() &&
                      voltage(backend, receiver) == 0U,
                  "late rollback cannot resurrect closed power");
        }
        CHECK(backend.begin_tune_power(backend.receiver_count(), System::ISDB_S, 15U).error() ==
                  Error::NOT_FOUND,
              "absent receiver cannot create power state");
        CHECK(backend.begin_tune_power(0U, System::ISDB_S, 15U).has_value(),
              "mock can prepare shutdown while ON");
        backend.request_stop();
        CHECK(backend.shutdown().has_value() && voltage(backend, 0U) == 0U,
              "shutdown clears synthetic power after stop request");
        CHECK(backend.begin_tune_power(0U, System::ISDB_S, 15U).error() == Error::NOT_READY,
              "shutdown cannot be re-energized");
    }
    return true;
}

bool service_and_ipc_preserve_lnb_state()
{
    for (std::size_t model = 0U; model < asicen::profile_count(); ++model) {
        asicen::MockTunerBackend backend(asicen::profiles()[model]);
        Nonce nonce;
        Time time;
        TunerService service(backend, nonce, time);
        const auto acquired = service.acquire(1U, 0U);
        CHECK(acquired.has_value(), "each model provides its mock satellite lease");
        for (const std::uint8_t requested : {15U, 0U, 15U}) {
            TuneRequestPayload tune{acquired.value().lease_id,
                                    System::ISDB_S,
                                    1049480U,
                                    0xffffU,
                                    0U,
                                    0U,
                                    requested,
                                    1000U};
            std::array<std::uint8_t, kMaxControlPayload> bytes{};
            const auto encoded = encode_payload(tune, {bytes.data(), bytes.size()});
            CHECK(encoded.has_value(), "LNB request encodes in ASICEN product IPC");
            const auto decoded = decode_tune_request_payload({bytes.data(), encoded.value()});
            CHECK(decoded && decoded.value().lnb_voltage == requested,
                  "product IPC preserves exact ON/OFF request");
            CHECK(service.tune(1U, decoded.value()).has_value() &&
                      voltage(backend, 0U) == requested,
                  "TunerService commits requested power after successful tune");
        }
        CHECK(service.release(1U, acquired.value().lease_id).has_value() &&
                  voltage(backend, 0U) == 0U,
              "service lease release clears synthetic ON");
    }
    return true;
}
}  // namespace

int main()
{
    if (!mock_transactions_and_model_boundaries()) {
        return 1;
    }
    if (!service_and_ipc_preserve_lnb_state()) {
        return 1;
    }
    std::cout << "product LNB IPC/mock contract tests passed (synthetic only)\n";
    return 0;
}
