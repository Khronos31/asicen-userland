// SPDX-License-Identifier: GPL-2.0-only
#include "posix_tuner_nonce_internal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc::posix;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) return false;                                                      \
    } while (false)

#define CHECK_RESULT(condition, failure)                                                     \
    do {                                                                                    \
        if (!(condition)) return failure;                                                    \
    } while (false)

struct ReadStep final {
    Result<EntropyReadResult> result = Result<EntropyReadResult>::success(
        EntropyReadResult{0U, false});
    std::uint8_t fill = 0U;
};

class FakeIo final : public TunerNonceIo {
public:
    Result<int> open_urandom() noexcept override
    {
        ++open_calls;
        return open_result;
    }

    Result<EntropyReadResult> read_entropy(
        int descriptor, std::uint8_t* output, std::size_t size) noexcept override
    {
        CHECK_RESULT(descriptor == 42 && output != nullptr && size > 0U,
                     Result<EntropyReadResult>::failure(Error::INVALID_ARGUMENT));
        if (next_step >= steps.size()) {
            return Result<EntropyReadResult>::failure(Error::INTERNAL);
        }
        const ReadStep& step = steps[next_step++];
        if (step.result) {
            const std::size_t bytes = step.result.value().bytes;
            if (bytes <= size) {
                for (std::size_t index = 0U; index < bytes; ++index) {
                    output[index] = static_cast<std::uint8_t>(step.fill + index);
                }
            }
        }
        return step.result;
    }

    Result<void> close_entropy(int descriptor) noexcept override
    {
        CHECK_RESULT(descriptor == 42, Result<void>::failure(Error::INVALID_ARGUMENT));
        ++close_calls;
        return close_result;
    }

    static Result<EntropyReadResult> bytes(std::size_t count, std::uint8_t /*fill*/) noexcept
    {
        return Result<EntropyReadResult>::success(EntropyReadResult{count, false});
    }

    static Result<EntropyReadResult> eintr() noexcept
    {
        return Result<EntropyReadResult>::success(EntropyReadResult{0U, true});
    }

    Result<int> open_result = Result<int>::success(42);
    Result<void> close_result = Result<void>::success();
    std::vector<ReadStep> steps;
    std::size_t next_step = 0U;
    std::size_t open_calls = 0U;
    std::size_t close_calls = 0U;

};

bool test_short_read_and_eintr()
{
    FakeIo io;
    io.steps.push_back(ReadStep{FakeIo::eintr(), 0U});
    io.steps.push_back(ReadStep{FakeIo::bytes(3U, 0x10U), 0x10U});
    io.steps.push_back(ReadStep{FakeIo::bytes(13U, 0x20U), 0x20U});
    const auto nonce = generate_tuner_nonce(io);
    CHECK(nonce && io.open_calls == 1U && io.close_calls == 1U);
    CHECK(nonce.value()[0] == 0x10U && nonce.value()[2] == 0x12U &&
          nonce.value()[3] == 0x20U && nonce.value()[15] == 0x2cU);
    return true;
}

bool test_eof_and_read_failure()
{
    FakeIo eof;
    eof.steps.push_back(ReadStep{FakeIo::bytes(0U, 0U), 0U});
    const auto eof_result = generate_tuner_nonce(eof);
    CHECK(!eof_result && eof_result.error() == Error::INTERNAL && eof.close_calls == 1U);

    FakeIo failure;
    failure.steps.push_back(ReadStep{Result<EntropyReadResult>::failure(Error::USB_IO), 0U});
    const auto failure_result = generate_tuner_nonce(failure);
    CHECK(!failure_result && failure_result.error() == Error::USB_IO &&
          failure.close_calls == 1U);
    return true;
}

bool test_open_and_close_failure()
{
    FakeIo open_failure;
    open_failure.open_result = Result<int>::failure(Error::INTERNAL);
    const auto result = generate_tuner_nonce(open_failure);
    CHECK(!result && result.error() == Error::INTERNAL && open_failure.close_calls == 0U);

    FakeIo close_failure;
    close_failure.steps.push_back(ReadStep{FakeIo::bytes(16U, 0x40U), 0x40U});
    close_failure.close_result = Result<void>::failure(Error::INTERNAL);
    const auto close_result = generate_tuner_nonce(close_failure);
    CHECK(!close_result && close_result.error() == Error::INTERNAL &&
          close_failure.close_calls == 1U);
    return true;
}

}  // namespace

bool run_posix_tuner_nonce_tests()
{
    return test_short_read_and_eintr() && test_eof_and_read_failure() &&
           test_open_and_close_failure();
}
