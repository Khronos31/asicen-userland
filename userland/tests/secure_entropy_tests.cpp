// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/secure_entropy.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct Step { asicen::EntropyReadStatus status; std::size_t bytes; };

class FakeEntropy final : public asicen::SecureEntropyIo {
public:
    explicit FakeEntropy(std::vector<Step> steps, bool open_ok = true,
                         bool close_ok = true)
        : steps_(std::move(steps)), open_ok_(open_ok), close_ok_(close_ok) {}
    bool open() noexcept override { ++opens; return open_ok_; }
    asicen::EntropyRead read(std::uint8_t* output, std::size_t size) noexcept override {
        if (next_ >= steps_.size()) return {asicen::EntropyReadStatus::Error, 0U};
        const Step step = steps_[next_++];
        if (step.status == asicen::EntropyReadStatus::Data) {
            const std::size_t count = step.bytes < size ? step.bytes : size;
            for (std::size_t i = 0U; i < count; ++i)
                output[i] = static_cast<std::uint8_t>(0x30U + offset_++);
            return {step.status, count};
        }
        return {step.status, step.bytes};
    }
    bool close() noexcept override { ++closes; return close_ok_; }
    unsigned opens = 0U;
    unsigned closes = 0U;
private:
    std::vector<Step> steps_;
    std::size_t next_ = 0U;
    std::uint8_t offset_ = 0U;
    bool open_ok_;
    bool close_ok_;
};

void test_partial_and_interrupted_fill() {
    FakeEntropy io({{asicen::EntropyReadStatus::Data, 3U},
                    {asicen::EntropyReadStatus::Interrupted, 0U},
                    {asicen::EntropyReadStatus::Data, 5U}});
    std::array<std::uint8_t, 8> output{};
    check(asicen::fill_secure_entropy_with_io(io, output.data(), output.size()),
          "partial reads and interruption fill exactly");
    check(output[0] == 0x30U && output[7] == 0x37U, "all returned bytes preserved");
    check(io.opens == 1U && io.closes == 1U, "successful source closed once");
}

void test_failures_clear_output_and_close() {
    {
        FakeEntropy io({{asicen::EntropyReadStatus::Data, 0U}});
        std::array<std::uint8_t, 8> output{};
        output.fill(0xa5U);
        check(!asicen::fill_secure_entropy_with_io(io, output.data(), output.size()),
              "zero-byte read fails");
        for (const auto byte : output) check(byte == 0U, "failure wipes partial output");
        check(io.closes == 1U, "opened source closed after read failure");
    }

    FakeEntropy partial_then_error({{asicen::EntropyReadStatus::Data, 3U},
                                    {asicen::EntropyReadStatus::Error, 0U}});
    std::array<std::uint8_t, 16> partial_output{};
    partial_output.fill(0xa5U);
    check(!asicen::fill_secure_entropy_with_io(partial_then_error,
                                               partial_output.data(),
                                               partial_output.size()),
          "read error after partial data fails fill");
    for (const auto byte : partial_output)
        check(byte == 0U, "late read error wipes all 16 bytes");
    check(partial_then_error.closes == 1U,
          "source closes once after late read error");

    FakeEntropy close_failure({{asicen::EntropyReadStatus::Data, 8U}}, true, false);
    std::array<std::uint8_t, 8> output{};
    output.fill(0x7fU);
    check(!asicen::fill_secure_entropy_with_io(close_failure, output.data(), output.size()),
          "close failure fails fill");
    for (const auto byte : output) check(byte == 0U, "close failure wipes output");

    FakeEntropy open_failure({}, false);
    output.fill(0x7fU);
    check(!asicen::fill_secure_entropy_with_io(open_failure, output.data(), output.size()),
          "open failure fails fill");
    for (const auto byte : output) check(byte == 0U, "open failure wipes output");
    check(open_failure.closes == 0U, "failed open is not closed");
}

void test_native_provider_returns_exact_length() {
    std::array<std::uint8_t, 16> output{};
    check(asicen::fill_secure_entropy(output.data(), output.size()),
          "native provider fills the full seed buffer");
}
}  // namespace

int main() {
    test_partial_and_interrupted_fill();
    test_failures_clear_output_and_close();
    test_native_provider_returns_exact_length();
    std::puts("secure entropy tests: PASS");
    return 0;
}
