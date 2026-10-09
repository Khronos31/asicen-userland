// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/device_profile.h"
#include "asicen/px4_mock_backend.h"
#include "px4/ipc.h"
#include "px4_ts_core.h"

#include <array>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace px4::userland;
using namespace px4::userland::cli;
using namespace px4::userland::ipc;

void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

Px4TsArguments parse(const std::vector<std::string>& options) {
    std::vector<const char*> args{"asicen-ts"};
    for (const auto& option : options) args.push_back(option.c_str());
    return parse_px4_ts_arguments(static_cast<int>(args.size()), args.data());
}

void cli_matches_reference_lnb_contract() {
    const std::vector<std::vector<std::string>> satellite_inputs{
        {"--instance", "lnb-test", "--receiver", "0", "--channel", "BS01_0"},
        {"--instance", "lnb-test", "--receiver", "0", "--channel", "CS2"},
        {"--instance", "lnb-test", "--receiver", "0", "--system", "isdb-s",
         "--frequency-khz", "1049480", "--slot", "0"},
        {"--instance", "lnb-test", "--receiver", "2", "--system", "isdb-s",
         "--frequency-khz", "1049480", "--stream-id", "42"},
    };
    for (const auto& input : satellite_inputs) {
        const auto defaults = parse(input);
        check(defaults.valid && defaults.lnb_voltage == 0U &&
              defaults.tune_timeout_ms == 10000U && defaults.bandwidth_hz == 0U,
              "satellite defaults retain upstream OFF and timeout contract");
        for (const auto* value : {"0", "15", "00", "015"}) {
            auto options = input;
            options.insert(options.end(), {"--lnb-voltage", value});
            const auto parsed = parse(options);
            check(parsed.valid && parsed.lnb_voltage == std::stoul(value),
                  "decimal 0/15 requests preserve upstream parsing");
        }
        for (const auto* value : {"1", "13", "18", "255", "256", "-1", "+15",
                                  "15V", "on", "off", "15.0", "", " 15", "15 "}) {
            auto options = input;
            options.insert(options.end(), {"--lnb-voltage", value});
            check(!parse(options).valid, "unsupported or malformed voltage is rejected");
        }
        auto missing = input;
        missing.push_back("--lnb-voltage");
        check(!parse(missing).valid, "missing LNB value is rejected");
        auto duplicate = input;
        duplicate.insert(duplicate.end(), {"--lnb-voltage", "0", "--lnb-voltage", "15"});
        check(!parse(duplicate).valid, "duplicate LNB option is rejected");
        for (const auto* invented : {"--lnb", "--lnb-power", "--allow-lnb-power"}) {
            auto options = input;
            options.insert(options.end(), {invented, "15"});
            check(!parse(options).valid, "unrelated LNB aliases are not introduced");
        }
    }
    const std::vector<std::string> terrestrial{
        "--instance", "lnb-test", "--receiver", "1", "--channel", "T27"};
    check(parse(terrestrial).valid && parse(terrestrial).lnb_voltage == 0U,
          "terrestrial default remains valid and OFF");
    for (const auto* value : {"0", "15"}) {
        auto options = terrestrial;
        options.insert(options.end(), {"--lnb-voltage", value});
        check(!parse(options).valid,
              "even explicit zero is a satellite-only CLI field upstream");
    }
    check(px4_ts_exit_status(Error::UNSUPPORTED, Px4TsFailureKind::ipc) == 3 &&
          px4_ts_exit_status(Error::USB_IO, Px4TsFailureKind::ipc) == 7 &&
          px4_ts_exit_status(Error::DISCONNECTED, Px4TsFailureKind::ipc) == 7,
          "unsupported and transport failure status remain honest");
}

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, kNonceLength>> generate() noexcept override {
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

std::uint8_t voltage(const asicen::MockTunerBackend& backend, std::uint8_t receiver) {
    const auto result = backend.simulated_lnb_voltage(receiver);
    check(result.has_value(), "mock state inspection uses a present receiver");
    return result.value();
}

void mock_transactions_and_model_boundaries() {
    for (std::size_t model = 0U; model < asicen::profile_count(); ++model) {
        const auto& profile = asicen::profiles()[model];
        asicen::MockTunerBackend backend(profile);
        for (std::uint8_t receiver = 0U; receiver < backend.receiver_count(); ++receiver) {
            check(voltage(backend, receiver) == 0U, "mock starts OFF");
            if (!backend.receiver_supports(receiver, System::ISDB_S)) {
                check(backend.begin_tune_power(receiver, System::ISDB_S, 15U).error() ==
                          Error::INVALID_ARGUMENT,
                      "terrestrial receiver cannot accept satellite ON");
                check(backend.begin_tune_power(receiver, System::ISDB_T, 15U).error() ==
                          Error::INVALID_ARGUMENT,
                      "terrestrial tune cannot request ON");
                continue;
            }
            check(backend.begin_tune_power(receiver, System::ISDB_S, 13U).error() ==
                      Error::INVALID_ARGUMENT,
                  "mock rejects unsupported voltage without a pending transaction");
            check(backend.begin_tune_power(receiver, System::ISDB_S, 15U).has_value() &&
                  voltage(backend, receiver) == 15U, "mock ON precedes acquisition");
            check(backend.begin_tune_power(receiver, System::ISDB_S, 0U).error() == Error::BUSY,
                  "overlapping tune power transaction is rejected");
            check(backend.rollback_tune_power(receiver).has_value() &&
                  voltage(backend, receiver) == 0U, "failed first ON rolls back to OFF");
            check(backend.begin_tune_power(receiver, System::ISDB_S, 15U).has_value() &&
                  backend.commit_tune_power(receiver).has_value(), "successful ON commits");
            check(backend.begin_tune_power(receiver, System::ISDB_S, 0U).has_value() &&
                  voltage(backend, receiver) == 15U,
                  "OFF retune preserves prior committed power until success");
            check(backend.rollback_tune_power(receiver).has_value() &&
                  voltage(backend, receiver) == 15U,
                  "failed OFF retune restores prior committed ON");
            check(backend.begin_tune_power(receiver, System::ISDB_S, 0U).has_value() &&
                  backend.commit_tune_power(receiver).has_value() &&
                  voltage(backend, receiver) == 0U, "successful OFF commits");
            check(backend.begin_tune_power(receiver, System::ISDB_S, 15U).has_value() &&
                  backend.close_receiver(receiver).has_value() &&
                  voltage(backend, receiver) == 0U, "close clears pending ON and state");
            check(backend.rollback_tune_power(receiver).has_value() &&
                  voltage(backend, receiver) == 0U, "late rollback cannot resurrect closed power");
        }
        check(backend.begin_tune_power(backend.receiver_count(), System::ISDB_S, 15U).error() ==
                  Error::NOT_FOUND, "absent receiver cannot create power state");
        check(backend.begin_tune_power(0U, System::ISDB_S, 15U).has_value(),
              "mock can prepare shutdown while ON");
        backend.request_stop();
        check(backend.shutdown().has_value() && voltage(backend, 0U) == 0U,
              "shutdown clears synthetic power after stop request");
        check(backend.begin_tune_power(0U, System::ISDB_S, 15U).error() == Error::NOT_READY,
              "shutdown cannot be re-energized");
    }
}

void service_and_ipc_preserve_lnb_state() {
    for (std::size_t model = 0U; model < asicen::profile_count(); ++model) {
        asicen::MockTunerBackend backend(asicen::profiles()[model]);
        Nonce nonce;
        Time time;
        TunerService service(backend, nonce, time);
        const auto acquired = service.acquire(1U, 0U);
        check(acquired.has_value(), "each model provides its mock satellite lease");
        for (const std::uint8_t requested : {15U, 0U, 15U}) {
            TuneRequestPayload tune{acquired.value().lease_id, System::ISDB_S,
                1049480U, 0xffffU, 0U, 0U, requested, 1000U};
            std::array<std::uint8_t, kMaxControlPayload> bytes{};
            const auto encoded = encode_payload(tune, {bytes.data(), bytes.size()});
            check(encoded.has_value(), "LNB request encodes in ASICEN product IPC");
            const auto decoded = decode_tune_request_payload({bytes.data(), encoded.value()});
            check(decoded && decoded.value().lnb_voltage == requested,
                  "product IPC preserves exact ON/OFF request");
            check(service.tune(1U, decoded.value()).has_value() &&
                  voltage(backend, 0U) == requested,
                  "TunerService commits requested power after successful tune");
        }
        check(service.release(1U, acquired.value().lease_id).has_value() &&
              voltage(backend, 0U) == 0U, "service lease release clears synthetic ON");
    }
}
}  // namespace

int main() {
    cli_matches_reference_lnb_contract();
    mock_transactions_and_model_boundaries();
    service_and_ipc_preserve_lnb_state();
    std::cout << "product LNB CLI/IPC/mock contract tests passed (synthetic only)\n";
    return 0;
}
