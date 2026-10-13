// SPDX-License-Identifier: GPL-2.0-only
// Offline diagnostic, not a CTest performance gate or hardware acceptance test.
#include "asicen/transport_transform.h"
#include "asicen/transport_capture.h"
#include "transport_transform_reference.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

constexpr std::size_t kPacketSize = 188;
constexpr std::size_t kPacketsPerBatch = (64U * 1024U) / kPacketSize;
constexpr std::size_t kBatchBytes = kPacketsPerBatch * kPacketSize;
constexpr std::size_t kMaxPairs = 101;
constexpr std::size_t kMaxBatches = 256;
constexpr std::size_t kTransferBytes = 65536;
using Clock = std::chrono::steady_clock;

std::uint32_t next_random(std::uint32_t& state)
{
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

bool parse_count(const char* value, std::size_t maximum, std::size_t* output)
{
    if (value == nullptr || *value == '\0') {
        return false;
    }
    std::size_t parsed = 0;
    for (; *value != '\0'; ++value) {
        if (*value < '0' || *value > '9') {
            return false;
        }
        const auto digit = static_cast<std::size_t>(*value - '0');
        if (parsed > (maximum - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    if (parsed == 0 || parsed > maximum) {
        return false;
    }
    *output = parsed;
    return true;
}

const char* host_arch()
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "other";
#endif
}

const char* host_os()
{
#if defined(__ANDROID__)
    return "Android";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "other";
#endif
}

void print_compiler()
{
#if defined(__clang__)
    std::printf("compiler=Clang %s\n", __clang_version__);
#elif defined(__GNUC__)
    std::printf("compiler=GCC %s\n", __VERSION__);
#elif defined(_MSC_VER)
    std::printf("compiler=MSVC %d\n", _MSC_VER);
#else
    std::puts("compiler=unknown");
#endif
#ifdef __OPTIMIZE__
    std::puts("compiler_optimization=enabled");
#else
    std::puts("compiler_optimization=not-reported (verify build flags)");
#endif
#ifdef NDEBUG
    std::puts("ndebug=1");
#else
    std::puts("ndebug=0");
#endif
}

bool run_scalar(const std::uint8_t* input, std::size_t batches,
                const transport_transform_reference::TransportMaterialV7& material,
                std::uint8_t* output, double* elapsed_ms)
{
    const auto begin = Clock::now();
    for (std::size_t batch = 0; batch < batches; ++batch) {
        for (std::size_t offset = 0; offset < kBatchBytes; offset += kPacketSize) {
            if (!transport_transform_reference::transform_transport_packet_v7(
                    input + offset, kPacketSize, material.first_des_key.data(),
                    material.second_des_key.data(), material.xor_state.data(),
                    output + batch * kBatchBytes + offset)) {
                return false;
            }
        }
    }
    *elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    return true;
}

bool run_prepared(const std::uint8_t* input, std::size_t batches,
                  const asicen::TransportTransformV7& transform,
                  std::uint8_t* output, double* elapsed_ms)
{
    const auto begin = Clock::now();
    for (std::size_t batch = 0; batch < batches; ++batch) {
        for (std::size_t offset = 0; offset < kBatchBytes; offset += kPacketSize) {
            if (!transform.transform_packet(input + offset, kPacketSize,
                                            output + batch * kBatchBytes + offset)) {
                return false;
            }
        }
    }
    *elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    return true;
}

bool check_all_output(const std::uint8_t* output, const std::uint8_t* reference,
                      std::size_t batches)
{
    for (std::size_t batch = 0; batch < batches; ++batch) {
        if (std::memcmp(output + batch * kBatchBytes, reference, kBatchBytes) != 0) {
            std::fputs("all-byte comparison failed; no payload data is printed\n", stderr);
            return false;
        }
    }
    return true;
}

bool benchmark(std::size_t pairs, std::size_t batches)
{
    std::array<std::uint8_t, 16> seed{};
    std::uint32_t random = 0xc6e2f871U;
    for (auto& byte : seed) {
        byte = static_cast<std::uint8_t>(next_random(random));
    }
    transport_transform_reference::TransportMaterialV7 material;
    if (!transport_transform_reference::derive_transport_material_v7(seed.data(), seed.size(),
                                                                     &material)) {
        return false;
    }
    asicen::TransportTransformV7 transform;
    if (!transform.prepare(material.first_des_key.data(), material.second_des_key.data(),
                           material.xor_state.data())) {
        return false;
    }
    const std::size_t total_bytes = batches * kBatchBytes;
    std::unique_ptr<std::uint8_t[]> input(new (std::nothrow) std::uint8_t[kBatchBytes]);
    std::unique_ptr<std::uint8_t[]> reference(new (std::nothrow) std::uint8_t[kBatchBytes]);
    std::unique_ptr<std::uint8_t[]> scalar(new (std::nothrow) std::uint8_t[total_bytes]);
    std::unique_ptr<std::uint8_t[]> optimized(new (std::nothrow) std::uint8_t[total_bytes]);
    if (!input || !reference || !scalar || !optimized) {
        std::fputs("benchmark allocation failed\n", stderr);
        return false;
    }
    for (std::size_t i = 0; i < kBatchBytes; ++i) {
        input[i] = static_cast<std::uint8_t>(next_random(random));
    }
    for (std::size_t offset = 0; offset < kBatchBytes; offset += kPacketSize) {
        input[offset] = 0x47;
        if (!transport_transform_reference::transform_transport_packet_v7(
                input.get() + offset, kPacketSize, material.first_des_key.data(),
                material.second_des_key.data(), material.xor_state.data(),
                reference.get() + offset)) {
            return false;
        }
    }

    // Warm both paths and fault in all output pages outside reported timings.
    double scalar_ms = 0;
    double optimized_ms = 0;
    if (!run_scalar(input.get(), batches, material, scalar.get(), &scalar_ms) ||
        !run_prepared(input.get(), batches, transform, optimized.get(), &optimized_ms) ||
        !check_all_output(scalar.get(), reference.get(), batches) ||
        !check_all_output(optimized.get(), reference.get(), batches)) {
        return false;
    }
    std::printf("host_os=%s host_arch=%s pointer_bits=%zu cxx=%ld\n", host_os(), host_arch(),
                sizeof(void*) * 8U, static_cast<long>(__cplusplus));
    print_compiler();
    std::printf("synthetic_only=1 hardware_claim=none packet_bytes=%zu packets_per_batch=%zu "
                "batch_bytes=%zu batches_per_run=%zu pairs=%zu\n", kPacketSize, kPacketsPerBatch,
                kBatchBytes, batches, pairs);
    std::puts("baseline=frozen-original-scalar-per-packet optimized=reused-prepared-context "
              "prepare_timed=0 equality=every-output-byte-outside-timing");
    std::puts("pair,order,scalar_ms,optimized_ms,scalar_mib_s,optimized_mib_s,speedup");
    std::array<double, kMaxPairs> ratios{};
    const double mib = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    for (std::size_t pair = 0; pair < pairs; ++pair) {
        bool success;
        if ((pair & 1U) == 0) {
            success = run_scalar(input.get(), batches, material, scalar.get(), &scalar_ms) &&
                      run_prepared(input.get(), batches, transform, optimized.get(), &optimized_ms);
        } else {
            success = run_prepared(input.get(), batches, transform, optimized.get(), &optimized_ms) &&
                      run_scalar(input.get(), batches, material, scalar.get(), &scalar_ms);
        }
        if (!success || scalar_ms <= 0 || optimized_ms <= 0 ||
            !check_all_output(scalar.get(), reference.get(), batches) ||
            !check_all_output(optimized.get(), reference.get(), batches)) {
            return false;
        }
        ratios[pair] = scalar_ms / optimized_ms;
        std::printf("%zu,%s,%.6f,%.6f,%.6f,%.6f,%.6f\n", pair + 1,
                    (pair & 1U) == 0 ? "scalar-first" : "optimized-first", scalar_ms,
                    optimized_ms, mib * 1000.0 / scalar_ms, mib * 1000.0 / optimized_ms,
                    ratios[pair]);
    }
    std::sort(ratios.begin(), ratios.begin() + static_cast<std::ptrdiff_t>(pairs));
    const auto median = pairs % 2 == 0
                            ? (ratios[pairs / 2 - 1] + ratios[pairs / 2]) / 2.0
                            : ratios[pairs / 2];
    std::printf("speedup_min=%.6f speedup_median=%.6f speedup_max=%.6f all_output_equal=1\n",
                ratios[0], median, ratios[pairs - 1]);
    return true;
}

// Independent copy of the pre-cache capture adapter, using the frozen scalar
// transform and derivation. The unchanged framer is intentionally shared so
// this comparison isolates the real capture path's transform/caching change.
class ScalarCaptureDecoder final {
public:
    ScalarCaptureDecoder(const std::uint8_t* seed, std::size_t seed_size)
    {
        valid_ = framer_.valid() && transport_transform_reference::derive_transport_material_v7(
                                      seed, seed_size, &material_);
    }
    bool valid() const { return valid_; }
    asicen::TsFramer::Counters counters() const noexcept { return framer_.counters(); }
    px4::userland::Result<void> push(px4::userland::ByteView input,
                                     asicen::TsFramer::PacketSink sink, void* context) noexcept
    {
        using px4::userland::Error;
        using px4::userland::Result;
        if (!valid_) {
            return Result<void>::failure(Error::NOT_READY);
        }
        if (sink == nullptr) {
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        }
        struct Adapter {
            const transport_transform_reference::TransportMaterialV7* material;
            asicen::TsFramer::PacketSink sink;
            void* context;
        } adapter{&material_, sink, context};
        return framer_.push(
            input,
            [](void* opaque, px4::userland::ByteView packet) noexcept {
                auto& target = *static_cast<Adapter*>(opaque);
                std::array<std::uint8_t, kPacketSize> decoded{};
                if (!transport_transform_reference::transform_transport_packet_v7(
                        packet.data, packet.size, target.material->first_des_key.data(),
                        target.material->second_des_key.data(), target.material->xor_state.data(),
                        decoded.data())) {
                    return Result<void>::failure(Error::PROTOCOL_ERROR);
                }
                return target.sink(target.context, {decoded.data(), decoded.size()});
            },
            &adapter);
    }

private:
    asicen::TsFramer framer_{asicen::TransportMode::AsicenMode7};
    transport_transform_reference::TransportMaterialV7 material_{};
    bool valid_ = false;
};

struct CollectedOutput {
    std::uint8_t* bytes;
    std::size_t capacity;
    std::size_t size;
};

px4::userland::Result<void> collect_packet(void* opaque,
                                          px4::userland::ByteView packet) noexcept
{
    auto& output = *static_cast<CollectedOutput*>(opaque);
    if (output.size > output.capacity || packet.size > output.capacity - output.size) {
        return px4::userland::Result<void>::failure(px4::userland::Error::SLOW_CONSUMER);
    }
    std::memcpy(output.bytes + output.size, packet.data, packet.size);
    output.size += packet.size;
    return px4::userland::Result<void>::success();
}

struct CaptureResult {
    double elapsed_ms = 0;
    std::size_t output_bytes = 0;
    std::array<asicen::TsFramer::Counters, kMaxBatches> counters{};
};

template <typename Decoder>
bool run_decoder(const std::array<std::uint8_t, 16>& seed, const std::uint8_t* input,
                 std::size_t chunks, std::uint8_t* output, CaptureResult* result)
{
    Decoder decoder(seed.data(), seed.size());
    if (!decoder.valid()) {
        return false;
    }
    CollectedOutput collected{output, chunks * kTransferBytes, 0};
    const auto begin = Clock::now();
    for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
        if (!decoder.push({input + chunk * kTransferBytes, kTransferBytes}, collect_packet,
                          &collected).has_value()) {
            return false;
        }
        result->counters[chunk] = decoder.counters();
    }
    result->elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    result->output_bytes = collected.size;
    return true;
}

bool check_decoder_output(const CaptureResult& scalar_result,
                          const CaptureResult& optimized_result,
                          const std::uint8_t* scalar, const std::uint8_t* optimized,
                          const std::uint8_t* expected, std::size_t chunks)
{
    const auto expected_bytes = (chunks * kTransferBytes / kPacketSize) * kPacketSize;
    if (scalar_result.output_bytes != expected_bytes ||
        optimized_result.output_bytes != expected_bytes ||
        std::memcmp(scalar, expected, expected_bytes) != 0 ||
        std::memcmp(optimized, expected, expected_bytes) != 0) {
        std::fputs("decoder all-byte comparison failed; no payload data is printed\n", stderr);
        return false;
    }
    for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
        const auto accepted_bytes = (chunk + 1) * kTransferBytes;
        const auto& left = scalar_result.counters[chunk];
        const auto& right = optimized_result.counters[chunk];
        if (left.input_bytes_accepted != right.input_bytes_accepted ||
            left.emitted_packets != right.emitted_packets ||
            left.discarded_sync_search_bytes != right.discarded_sync_search_bytes ||
            left.sync_loss_events != right.sync_loss_events ||
            left.buffered_bytes != right.buffered_bytes ||
            left.input_bytes_accepted != accepted_bytes ||
            left.emitted_packets != accepted_bytes / kPacketSize ||
            left.discarded_sync_search_bytes != 0 || left.sync_loss_events != 0 ||
            left.buffered_bytes != accepted_bytes % kPacketSize) {
            std::fputs("decoder counter/partial-carry comparison failed\n", stderr);
            return false;
        }
    }
    return true;
}

bool benchmark_decoder(std::size_t pairs, std::size_t chunks)
{
    std::array<std::uint8_t, 16> seed{};
    std::uint32_t random = 0x417aed83U;
    for (auto& byte : seed) {
        byte = static_cast<std::uint8_t>(next_random(random));
    }
    transport_transform_reference::TransportMaterialV7 material;
    if (!transport_transform_reference::derive_transport_material_v7(seed.data(), seed.size(),
                                                                     &material)) {
        return false;
    }
    const auto total_bytes = chunks * kTransferBytes;
    std::unique_ptr<std::uint8_t[]> input(new (std::nothrow) std::uint8_t[total_bytes]);
    std::unique_ptr<std::uint8_t[]> reference(new (std::nothrow) std::uint8_t[total_bytes]);
    std::unique_ptr<std::uint8_t[]> scalar(new (std::nothrow) std::uint8_t[total_bytes]);
    std::unique_ptr<std::uint8_t[]> optimized(new (std::nothrow) std::uint8_t[total_bytes]);
    if (!input || !reference || !scalar || !optimized) {
        return false;
    }
    for (std::size_t offset = 0; offset < total_bytes; ++offset) {
        input[offset] = offset % kPacketSize == 0
                            ? 0x47
                            : static_cast<std::uint8_t>(next_random(random));
    }
    for (std::size_t offset = 0; offset + kPacketSize <= total_bytes; offset += kPacketSize) {
        if (!transport_transform_reference::transform_transport_packet_v7(
                input.get() + offset, kPacketSize, material.first_des_key.data(),
                material.second_des_key.data(), material.xor_state.data(),
                reference.get() + offset)) {
            return false;
        }
    }
    CaptureResult scalar_result;
    CaptureResult optimized_result;
    if (!run_decoder<ScalarCaptureDecoder>(seed, input.get(), chunks, scalar.get(), &scalar_result) ||
        !run_decoder<asicen::TransportCaptureDecoderV7>(seed, input.get(), chunks, optimized.get(),
                                                       &optimized_result) ||
        !check_decoder_output(scalar_result, optimized_result, scalar.get(), optimized.get(),
                              reference.get(), chunks)) {
        return false;
    }
    std::printf("host_os=%s host_arch=%s pointer_bits=%zu cxx=%ld\n", host_os(), host_arch(),
                sizeof(void*) * 8U, static_cast<long>(__cplusplus));
    print_compiler();
    std::printf("synthetic_only=1 hardware_claim=none path=decoder transfer_bytes=%zu "
                "chunks_per_run=%zu pairs=%zu emitted_bytes=%zu final_carry_bytes=%zu\n",
                kTransferBytes, chunks, pairs, scalar_result.output_bytes, total_bytes % kPacketSize);
    std::puts("baseline=framer-plus-frozen-scalar-adapter optimized=TransportCaptureDecoderV7 "
              "constructor_timed=0 equality=all-output-bytes counter_equality=per-chunk-snapshots");
    std::puts("pair,order,scalar_ms,optimized_ms,scalar_mib_s,optimized_mib_s,speedup");
    std::array<double, kMaxPairs> ratios{};
    const double mib = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    for (std::size_t pair = 0; pair < pairs; ++pair) {
        bool success;
        if ((pair & 1U) == 0) {
            success = run_decoder<ScalarCaptureDecoder>(seed, input.get(), chunks, scalar.get(),
                                                         &scalar_result) &&
                      run_decoder<asicen::TransportCaptureDecoderV7>(
                          seed, input.get(), chunks, optimized.get(), &optimized_result);
        } else {
            success = run_decoder<asicen::TransportCaptureDecoderV7>(
                          seed, input.get(), chunks, optimized.get(), &optimized_result) &&
                      run_decoder<ScalarCaptureDecoder>(seed, input.get(), chunks, scalar.get(),
                                                         &scalar_result);
        }
        if (!success || scalar_result.elapsed_ms <= 0 || optimized_result.elapsed_ms <= 0 ||
            !check_decoder_output(scalar_result, optimized_result, scalar.get(), optimized.get(),
                                  reference.get(), chunks)) {
            return false;
        }
        ratios[pair] = scalar_result.elapsed_ms / optimized_result.elapsed_ms;
        std::printf("%zu,%s,%.6f,%.6f,%.6f,%.6f,%.6f\n", pair + 1,
                    (pair & 1U) == 0 ? "scalar-first" : "optimized-first", scalar_result.elapsed_ms,
                    optimized_result.elapsed_ms, mib * 1000.0 / scalar_result.elapsed_ms,
                    mib * 1000.0 / optimized_result.elapsed_ms, ratios[pair]);
    }
    std::sort(ratios.begin(), ratios.begin() + static_cast<std::ptrdiff_t>(pairs));
    const auto median = pairs % 2 == 0
                            ? (ratios[pairs / 2 - 1] + ratios[pairs / 2]) / 2.0
                            : ratios[pairs / 2];
    std::printf("speedup_min=%.6f speedup_median=%.6f speedup_max=%.6f "
                "all_output_equal=1 all_counters_equal=1\n", ratios[0], median, ratios[pairs - 1]);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::size_t pairs = 9;
    std::size_t batches = 8;
    bool decoder_mode = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--decoder") == 0) {
            decoder_mode = true;
        } else if (index + 1 < argc && std::strcmp(argv[index], "--pairs") == 0 &&
            parse_count(argv[index + 1], kMaxPairs, &pairs)) {
            ++index;
        } else if (index + 1 < argc && std::strcmp(argv[index], "--batches") == 0 &&
                   parse_count(argv[index + 1], kMaxBatches, &batches)) {
            ++index;
        } else {
            std::fputs("usage: asicen-transport-transform-benchmark [--decoder] [--pairs 1..101] "
                       "[--batches 1..256]\n", stderr);
            return 2;
        }
    }
    return (decoder_mode ? benchmark_decoder(pairs, batches) : benchmark(pairs, batches)) ? 0 : 1;
}
