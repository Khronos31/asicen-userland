// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/device_profile.h"
#include "asicen/product_profile.h"
#include "asicen/px4_mock_backend.h"
#include "px4/ipc.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
using namespace px4::userland;
using namespace px4::userland::ipc;

#define CHECK(condition, message)                    \
    do {                                             \
        if (!(condition)) {                          \
            std::cerr << "FAIL: " << message << '\n'; \
            return false;                            \
        }                                            \
    } while (false)

bool profile_metadata_and_mock_routing()
{
    constexpr std::array<std::uint16_t, 5> pids{1U, 3U, 4U, 5U, 6U};
    constexpr std::array<std::uint8_t, 5> counts{1U, 2U, 4U, 4U, 4U};
    constexpr std::array<asicen::FrontendFamily, 5> families{
        asicen::FrontendFamily::S3u, asicen::FrontendFamily::S3u2,
        asicen::FrontendFamily::W3u3, asicen::FrontendFamily::W3u3,
        asicen::FrontendFamily::Nmi};
    CHECK(asicen::profile_count() == pids.size(), "all five USB models have profiles");
    for (std::size_t i = 0; i < pids.size(); ++i) {
        const auto* device = asicen::find_profile(0x0b06U, pids[i]);
        CHECK(device != nullptr, "runtime USB identity resolves");
        CHECK(device->enclosure_receiver_count == counts[i] &&
              device->expected_runtime_functions == asicen::profile::usb_function_count(counts[i]) &&
              device->local_lane_count == 2U && device->combined_isdb_ts == (i == 0U) &&
              device->frontend_family == families[i], "model topology and frontend family");
        CHECK(asicen::find_profile(device->model_id) == device &&
              asicen::find_profile_by_model(device->model_key) == device &&
              asicen::find_profile_by_model(device->model) == device,
              "ID, canonical key and display name resolve to same immutable profile");
        CHECK(std::string_view(asicen::model_id_name(device->model_id)) == device->model_key &&
              std::string_view(asicen::frontend_family_name(device->frontend_family)) != "unknown",
              "stable names exist for every known model/family");
        CHECK(asicen::profile_runtime_supported(*device) == device->source_supported,
              "canonical profile readiness follows its explicit source gate");

        // Mock behavior tests topology even when actual hardware remains gated.
        asicen::MockTunerBackend backend(*device);
        CHECK(backend.receiver_count() == counts[i], "mock receives selected model count");
        for (std::uint8_t r = 0; r < counts[i]; ++r) {
            const bool satellite = counts[i] == 1U || r % 2U == 0U;
            const bool terrestrial = counts[i] == 1U || r % 2U == 1U;
            CHECK(backend.receiver_supports(r, System::ISDB_S) == satellite &&
                  backend.receiver_supports(r, System::ISDB_T) == terrestrial,
                  "mock receiver system routing matches profile");
            CHECK(!backend.receiver_supports(r, System::ISDB_T_OR_S),
                  "combined system describes capability, never a tune operation");
            CHECK(backend.open_receiver(r).has_value(), "mock receiver opens");
            CHECK(backend.tune_terrestrial(r, 557142U, 1000U).has_value() == terrestrial &&
                  backend.tune_satellite(r, 1049480U, 1000U).has_value() == satellite,
                  "mock tuning follows model system capability");
            CHECK(backend.close_receiver(r).has_value(), "mock receiver closes");
        }
        CHECK(!backend.receiver_supports(counts[i], System::ISDB_T) &&
              backend.open_receiver(counts[i]).error() == Error::NOT_FOUND,
              "mock rejects absent receiver");
        backend.request_stop();
        CHECK(backend.open_receiver(0U).error() == Error::NOT_READY,
              "model-aware mock retains sticky shutdown");
    }
    CHECK(asicen::find_profile_by_model("PX-W3U3-V2") == asicen::find_profile(asicen::ModelId::W3u3V2) &&
          asicen::find_profile_by_model("w3u3v2") == asicen::find_profile(asicen::ModelId::W3u3V2),
          "V2 spelling aliases resolve without falling back to W3U3");
    CHECK(asicen::find_profile_by_model("") == nullptr &&
          asicen::find_profile_by_model("w3u4") == nullptr &&
          asicen::find_profile(0x0b06U, 0x9999U) == nullptr,
          "unknown models and IDs stay unknown");
    const asicen::DeviceProfile legacy{0x0b06U, 0x0005U, "legacy", 4U, 2U, 2U, false};
    CHECK(!asicen::profile_runtime_supported(legacy),
          "legacy aggregate defaults cannot silently enable hardware writes");
    auto wrong_family = *asicen::find_profile(asicen::ModelId::W3u3);
    wrong_family.frontend_family = asicen::FrontendFamily::Nmi;
    CHECK(!asicen::profile_runtime_supported(wrong_family), "mismatched family is fail-closed");
    auto wrong_topology = *asicen::find_profile(asicen::ModelId::W3u3);
    wrong_topology.enclosure_receiver_count = 1U;
    CHECK(!asicen::profile_runtime_supported(wrong_topology), "mismatched topology is fail-closed");
    asicen::MockTunerBackend invalid_count(3U);
    CHECK(invalid_count.receiver_count() == 0U &&
          !invalid_count.receiver_supports(0U, System::ISDB_S),
          "invalid mock topology cannot expose receivers");
    return true;
}

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, kNonceLength>> generate() noexcept override {
        std::array<std::uint8_t, kNonceLength> bytes{};
        bytes[0] = ++sequence_;
        return Result<std::array<std::uint8_t, kNonceLength>>::success(bytes);
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

bool combined_receiver_has_one_exclusive_lease()
{
    asicen::MockTunerBackend backend(*asicen::find_profile(asicen::ModelId::S3u));
    Nonce nonce;
    Time time;
    TunerService service(backend, nonce, time);
    const auto acquired = service.acquire(1U, 0U);
    CHECK(acquired.has_value(), "S3U shared receiver acquires once");
    CHECK(service.acquire(2U, 0U).error() == Error::BUSY,
          "S3U T/S modes cannot acquire independent simultaneous leases");
    CHECK(!service.acquire(2U, 1U), "S3U has no second logical receiver");
    const auto lease = acquired.value().lease_id;
    CHECK(service.tune(1U, TuneRequestPayload{lease, System::ISDB_T,
              557142U, 0xffffU, 0xffffU, 6000000U, 0U, 500U}).has_value(),
          "S3U lease accepts terrestrial tune");
    CHECK(service.tune(1U, TuneRequestPayload{lease, System::ISDB_S,
              1049480U, 0xffffU, 0U, 0U, 0U, 500U}).has_value(),
          "same S3U lease switches to satellite tune");
    CHECK(service.release(1U, lease).has_value(), "S3U lease releases");
    const auto next = service.acquire(2U, 0U);
    CHECK(next.has_value(), "another client can acquire only after release");
    CHECK(service.release(2U, next.value().lease_id).has_value(), "next S3U lease releases");
    return true;
}

bool dynamic_list_roundtrips_and_rejects_malformed_shapes()
{
    for (const std::uint8_t count : {1U, 2U, 4U}) {
        const auto records = receiver_records(count, count == 1U);
        CHECK(records.has_value(), "dynamic ASICEN receiver records accepted");
        ListResponsePayload list{};
        list.generation = 1U;
        list.ready = 1U;
        list.usb_present_mask = asicen::profile::usb_present_mask(count);
        list.receiver_count = count;
        list.receivers = records.value();
        for (std::uint8_t r = 0U; r < count; ++r) {
            const auto& record = list.receivers[r];
            CHECK(record.global_id == r && record.dev_id == r / 2U + 1U &&
                  record.local_id == r % 2U &&
                  record.system == (count == 1U ? System::ISDB_T_OR_S :
                      r % 2U == 0U ? System::ISDB_S : System::ISDB_T),
                  "LIST model topology has exact receiver descriptors");
        }
        std::array<std::uint8_t, kMaxControlPayload> bytes{};
        const auto encoded = encode_payload(list, {bytes.data(), bytes.size()});
        CHECK(encoded.has_value(), "model LIST encodes");
        const auto decoded = decode_list_response_payload({bytes.data(), encoded.value()});
        CHECK(decoded && decoded.value().receiver_count == count &&
              decoded.value().usb_present_mask == list.usb_present_mask &&
              decoded.value().receivers[0].system == list.receivers[0].system,
              "model LIST count/mask/system round trips");
        const std::uint8_t first_system = bytes[17U];
        bytes[17U] = static_cast<std::uint8_t>(System::ISDB_T);
        CHECK(!decode_list_response_payload({bytes.data(), encoded.value()}),
              "wrong first system is rejected on decode");
        bytes[17U] = first_system;
        if (count < 4U) {
            bytes[11U] = 0x03U;
            CHECK(!decode_list_response_payload({bytes.data(), encoded.value()}),
                  "one-function LIST rejects second USB mask bit on decode");
            list.usb_present_mask = 0x03U;
            CHECK(!encode_payload(list, {bytes.data(), bytes.size()}),
                  "one-function LIST rejects second USB mask bit on encode");
            list.usb_present_mask = 0x01U;
        }
        list.receivers[0].local_id = 1U;
        CHECK(!encode_payload(list, {bytes.data(), bytes.size()}),
              "inconsistent receiver local mapping rejected");
    }
    for (const std::uint8_t count : {0U, 3U, 5U, 8U, 255U})
        CHECK(!receiver_records(count), "non-ASICEN count rejected");
    return true;
}
}  // namespace

int main()
{
    if (!profile_metadata_and_mock_routing() ||
        !dynamic_list_roundtrips_and_rejects_malformed_shapes() ||
        !combined_receiver_has_one_exclusive_lease()) {
        return 1;
    }
    std::cout << "model profile/IPC tests passed\n";
    return 0;
}
