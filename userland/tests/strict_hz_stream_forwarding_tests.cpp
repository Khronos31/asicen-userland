// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_stream_session.h"
#include "asicen/device_profile.h"
#include "asicen/libusb_hardware_backend.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>

namespace {
using namespace px4::userland;

bool same_tune_record(const dtv::userland::detail::TuneRecord& left,
                      const dtv::userland::detail::TuneRecord& right)
{
    return left.system == right.system && left.frequency_khz == right.frequency_khz &&
           left.stream_id == right.stream_id && left.slot == right.slot &&
           left.lnb_voltage == right.lnb_voltage && left.timeout_ms == right.timeout_ms &&
           left.entrance == right.entrance && left.requested_hz == right.requested_hz &&
           left.programmed_hz.present == right.programmed_hz.present &&
           left.programmed_hz.value == right.programmed_hz.value &&
           left.verification == right.verification && left.lock.state == right.lock.state &&
           left.lock.reason == right.lock.reason && left.lock.value == right.lock.value &&
           left.lock.sampled_at_ns == right.lock.sampled_at_ns;
}

class Frontend final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override { return 2U; }
    bool receiver_supports(std::uint8_t receiver, ipc::System system) const noexcept override
    {
        return (receiver == 0U && system == ipc::System::ISDB_S) ||
               (receiver == 1U && system == ipc::System::ISDB_T);
    }
    Result<void> open_receiver(std::uint8_t) noexcept override { return Result<void>::success(); }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        ++khz_calls;
        return Result<void>::success();
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        ++khz_calls;
        return Result<void>::success();
    }
    Result<TunerHzProgramming> tune_terrestrial_hz(std::uint8_t receiver, std::uint64_t hz,
                                                   std::uint32_t timeout) noexcept override
    {
        ++terrestrial_hz_calls;
        last_receiver = receiver;
        last_hz = hz;
        last_timeout = timeout;
        return Result<TunerHzProgramming>::success(programming);
    }
    Result<TunerHzProgramming> tune_satellite_hz(std::uint8_t receiver, std::uint64_t hz,
                                                 std::uint32_t timeout) noexcept override
    {
        ++satellite_hz_calls;
        last_receiver = receiver;
        last_hz = hz;
        last_timeout = timeout;
        return Result<TunerHzProgramming>::success(programming);
    }
    TunerStrictProfile strict_profile(std::uint8_t receiver,
                                      ipc::System system) const noexcept override
    {
        if (profile_source != nullptr) return profile_source->strict_profile(receiver, system);
        if (!receiver_supports(receiver, system)) return {};
        TunerStrictProfile profile{};
        profile.native_hz = true;
        profile.min_hz = system == ipc::System::ISDB_T ? 40000000U : 1049480000U;
        profile.max_hz = system == ipc::System::ISDB_T ? 889999999U : 2053000000U;
        profile.slot_count = system == ipc::System::ISDB_S ? 8U : 0U;
        return profile;
    }
    Result<bool> is_locked(std::uint8_t, ipc::System) noexcept override
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
    Result<void> close_receiver(std::uint8_t) noexcept override { return Result<void>::success(); }

    unsigned khz_calls = 0U;
    unsigned terrestrial_hz_calls = 0U;
    unsigned satellite_hz_calls = 0U;
    std::uint8_t last_receiver = 0xffU;
    std::uint64_t last_hz = 0U;
    std::uint32_t last_timeout = 0U;
    TunerHzProgramming programming{};
    const asicen::LibusbW3u3Hardware* profile_source = nullptr;
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

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return 77U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

class Source final : public asicen::StreamCaptureSource {
public:
    Result<void> prepare(std::uint8_t, ipc::System,
                         const std::atomic<bool>&) noexcept override
    {
        return Result<void>::success();
    }
    asicen::CaptureRunResult run(const std::atomic<bool>&,
                                 bool (*)(void*, const std::uint8_t*, std::size_t),
                                 void*) noexcept override
    {
        return asicen::CaptureRunResult::cancelled;
    }
    void interrupt() noexcept override {}
    Result<void> stop() noexcept override { return Result<void>::success(); }
};

bool check(bool condition, const char* message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

}  // namespace

int main()
{
    Frontend frontend;
    Source source;
    asicen::HardwareStreamService stream(frontend, source,
        asicen::HardwareStreamService::TestQueueCapacityBytes{188U});
    frontend.programming.programmed_hz = dtv::userland::wire::Optional<std::uint64_t>{true,
                                                                                       473143000U};

    const auto terrestrial = stream.tune_terrestrial_hz(1U, 473143123U, 2500U);
    if (!check(terrestrial && terrestrial.value().programmed_hz.present &&
               terrestrial.value().programmed_hz.value == 473143000U,
               "terrestrial programming result is forwarded")) return 1;
    if (!check(frontend.terrestrial_hz_calls == 1U && frontend.last_receiver == 1U &&
               frontend.last_hz == 473143123U && frontend.last_timeout == 2500U,
               "terrestrial Hz and timeout are forwarded exactly")) return 1;

    const auto satellite = stream.tune_satellite_hz(0U, 1049480000U, 3000U);
    if (!check(satellite && frontend.satellite_hz_calls == 1U && frontend.last_receiver == 0U &&
               frontend.last_hz == 1049480000U && frontend.last_timeout == 3000U,
               "satellite IF Hz and timeout are forwarded exactly")) return 1;

    const auto terrestrial_profile = stream.strict_profile(1U, ipc::System::ISDB_T);
    const auto satellite_profile = stream.strict_profile(0U, ipc::System::ISDB_S);
    if (!check(terrestrial_profile.native_hz && terrestrial_profile.min_hz == 40000000U &&
               terrestrial_profile.max_hz == 889999999U && satellite_profile.native_hz &&
               satellite_profile.slot_count == 8U,
               "immutable frontend profiles are forwarded")) return 1;

    const auto unsupported = stream.tune_terrestrial_hz(0U, 473143123U, 2500U);
    if (!check(!unsupported && unsupported.error() == Error::UNSUPPORTED && frontend.khz_calls == 0U &&
               frontend.terrestrial_hz_calls == 1U,
               "unsupported route does not fall back to kHz")) return 1;

    const auto* s3u_profile = asicen::find_profile(asicen::ModelId::S3u);
    const auto* v2_profile = asicen::find_profile(asicen::ModelId::W3u3V2);
    if (!check(s3u_profile != nullptr && v2_profile != nullptr,
               "A strict profile source models are available")) return 1;
    asicen::LibusbW3u3Hardware s3u(nullptr, {}, {}, {}, {}, s3u_profile);
    const auto* s3u2_profile = asicen::find_profile(asicen::ModelId::S3u2);
    const auto* w3u2_profile = asicen::find_profile(asicen::ModelId::W3u2);
    const auto* w3u3_profile = asicen::find_profile(asicen::ModelId::W3u3);
    asicen::LibusbW3u3Hardware v2_backend(nullptr, {}, {}, {}, {}, v2_profile);
    if (!check(s3u2_profile != nullptr && w3u2_profile != nullptr && w3u3_profile != nullptr,
               "A all reviewed model profiles are available")) return 1;
    asicen::LibusbW3u3Hardware s3u2(nullptr, {}, {}, {}, {}, s3u2_profile);
    asicen::LibusbW3u3Hardware w3u2(nullptr, {}, {}, {}, {}, w3u2_profile);
    asicen::LibusbW3u3Hardware w3u3(nullptr, {}, {}, {}, {}, w3u3_profile);
    const auto a_legacy_t = s3u.strict_profile(0U, ipc::System::ISDB_T);
    const auto a_legacy_s = s3u.strict_profile(0U, ipc::System::ISDB_S);
    const auto a_v2_t = v2_backend.strict_profile(1U, ipc::System::ISDB_T);
    const auto a_v2_s = v2_backend.strict_profile(0U, ipc::System::ISDB_S);
    if (!check(a_legacy_t.native_hz && a_legacy_t.min_hz == 40000000U &&
               a_legacy_t.max_hz == 889999999U && a_legacy_s.row_sets == 2U &&
               a_legacy_s.rows[0].first_hz == 1049480000U &&
               a_legacy_s.rows[0].step_hz == 38360000U &&
               a_legacy_s.rows[0].count == 12U &&
               a_legacy_s.rows[1].first_hz == 1613000000U &&
               a_legacy_s.rows[1].step_hz == 40000000U &&
               a_legacy_s.rows[1].count == 12U && a_legacy_s.slot_count == 8U,
               "A legacy domains and exact satellite rows are exposed")) return 1;
    if (!check(a_v2_t.native_hz && a_v2_t.min_hz == 90000000U &&
               a_v2_t.max_hz == 770000000U && a_v2_s.native_hz &&
               a_v2_s.row_sets == 2U && a_v2_s.slot_count == 8U &&
               a_v2_s.lnb_voltage_mask == 0x03U &&
               a_legacy_s.lnb_voltage_mask == 0U,
               "A V2 domains and exact satellite rows are exposed")) return 1;
    if (!check(s3u.strict_profile(0U, ipc::System::ISDB_S).lnb_voltage_mask == 0U &&
               s3u2.strict_profile(0U, ipc::System::ISDB_S).lnb_voltage_mask == 0U &&
               w3u2.strict_profile(0U, ipc::System::ISDB_S).lnb_voltage_mask == 0x03U &&
               w3u3.strict_profile(0U, ipc::System::ISDB_S).lnb_voltage_mask == 0x03U &&
               v2_backend.strict_profile(0U, ipc::System::ISDB_S).lnb_voltage_mask == 0x03U &&
               s3u.strict_profile(0U, ipc::System::ISDB_T).lnb_voltage_mask == 0U,
               "A model profiles distinguish no GPIO from 0/15 V LNB control")) return 1;

    Frontend facade_frontend;
    facade_frontend.profile_source = &s3u;
    Nonce nonce;
    Time time;
    TunerService facade(facade_frontend, nonce, time);
    if (!check(static_cast<bool>(facade.configure_strict_capabilities()) &&
               static_cast<bool>(facade.acquire(1U, 0U)),
               "A actual model profile can configure the prepared facade without USB")) return 1;
    TunerHzTuneRequest facade_request{};
    facade_request.lease_id = 1U;
    facade_request.system = ipc::System::ISDB_S;
    facade_request.frequency_hz = 1049480000U;
    facade_request.selection = dtv::userland::wire::Selection::slot;
    facade_request.timeout_ms = 1000U;
    const auto absent_lnb = facade.prepare_tune_hz(0U, facade_request);
    const auto absent_admission = facade.try_admit_prepared_tune(1U, absent_lnb, 77U);
    if (!check(absent_admission && absent_admission.value().receiver == 0U &&
               absent_admission.value().request.requested_hz == 1049480000U &&
               absent_admission.value().accepted_at_ms == 77U &&
               static_cast<bool>(facade.execute_admitted_tune(absent_admission.value())),
               "A model profile preserves absent LNB and prepared receiver facts")) return 1;
    const auto current_before_reject = facade.tune_metadata(0U);
    const auto before_reject = facade.status();
    const unsigned backend_calls_before_reject = facade_frontend.khz_calls +
        facade_frontend.terrestrial_hz_calls + facade_frontend.satellite_hz_calls;
    if (!check(current_before_reject && current_before_reject.value().current_valid &&
               current_before_reject.value().revision != 0U &&
               current_before_reject.value().current_revision ==
                   current_before_reject.value().revision &&
               current_before_reject.value().current.requested_hz == 1049480000U,
               "A successful absent-LNB tune seeds retained current metadata")) return 1;
    facade_request.lnb_voltage = {true, 0U};
    const auto explicit_zero = facade.prepare_tune_hz(0U, facade_request);
    const auto rejected_zero = facade.try_admit_prepared_tune(1U, explicit_zero, 77U);
    const auto after_reject = facade.status();
    const auto current_after_reject = facade.tune_metadata(0U);
    const unsigned backend_calls_after_reject = facade_frontend.khz_calls +
        facade_frontend.terrestrial_hz_calls + facade_frontend.satellite_hz_calls;
    if (!check(!rejected_zero && rejected_zero.error() == Error::UNSUPPORTED &&
               before_reject && after_reject && current_after_reject &&
               current_after_reject.value().revision == current_before_reject.value().revision &&
               current_after_reject.value().current_valid ==
                   current_before_reject.value().current_valid &&
               current_after_reject.value().current_revision ==
                   current_before_reject.value().current_revision &&
               same_tune_record(current_after_reject.value().current,
                                current_before_reject.value().current) &&
               before_reject.value().generation == after_reject.value().generation &&
               backend_calls_after_reject == backend_calls_before_reject,
               "A S3U explicit 0 V is rejected during preparation before backend calls")) return 1;

    for (std::uint32_t row = 0U; row < 12U; ++row) {
        const std::uint64_t if_hz = 1049480000U +
                                    static_cast<std::uint64_t>(row) * 38360000U;
        std::uint64_t rf_hz = 0U;
        if (!check(asicen::w3u3_satellite_if_hz_to_rf_hz(if_hz, &rf_hz) &&
                   rf_hz == if_hz + 10678000000U &&
                   !asicen::w3u3_satellite_if_hz_to_rf_hz(if_hz - 1U, &rf_hz) &&
                   !asicen::w3u3_satellite_if_hz_to_rf_hz(if_hz + 1U, &rf_hz),
                   "A exact first satellite row and adjacent Hz gate")) return 1;
    }
    for (std::uint32_t row = 0U; row < 12U; ++row) {
        const std::uint64_t if_hz = 1613000000U +
                                    static_cast<std::uint64_t>(row) * 40000000U;
        std::uint64_t rf_hz = 0U;
        if (!check(asicen::w3u3_satellite_if_hz_to_rf_hz(if_hz, &rf_hz) &&
                   rf_hz == if_hz + 10678000000U &&
                   !asicen::w3u3_satellite_if_hz_to_rf_hz(if_hz - 1U, &rf_hz) &&
                   !asicen::w3u3_satellite_if_hz_to_rf_hz(if_hz + 1U, &rf_hz),
                   "A exact second satellite row and adjacent Hz gate")) return 1;
    }
    const struct CenterVector { std::uint64_t requested; std::uint64_t expected; } legacy[] = {
        {164999999U, 164143000U}, {165000000U, 167143000U},
        {167999999U, 167143000U}, {168000000U, 168143000U},
        {194999999U, 194143000U}, {195000000U, 195143000U},
        {197999999U, 195143000U}, {198000000U, 198143000U},
        {470999999U, 470143000U}, {471000000U, 473143000U},
        {473999999U, 473143000U}, {474000000U, 474143000U},
        {889999999U, 889143000U}};
    for (const auto& vector : legacy) {
        if (!check(asicen::terrestrial_tune_center_hz(vector.requested) == vector.expected,
                   "A legacy terrestrial literal center vector")) return 1;
    }
    const struct CenterVector v2_centers[] = {
        {89999999U, 0U}, {90000000U, 90143000U}, {90000001U, 90143000U},
        {164999999U, 164143000U}, {165000000U, 167143000U},
        {167143000U, 167143000U}, {167143001U, 167143000U},
        {195142999U, 195143000U}, {195143000U, 195143000U},
        {197143000U, 195143000U}, {197143001U, 197143000U},
        {471142999U, 471143000U}, {471143000U, 473143000U},
        {473143000U, 473143000U}, {473143001U, 473143000U},
        {769999999U, 769143000U}, {770000000U, 770143000U},
        {770000001U, 0U}};
    for (const auto& vector : v2_centers) {
        if (!check(asicen::v2_terrestrial_center_hz(vector.requested) == vector.expected,
                   "A V2 terrestrial literal center vector")) return 1;
    }
    return 0;
}
