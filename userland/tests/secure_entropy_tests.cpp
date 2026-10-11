// SPDX-License-Identifier: GPL-2.0-only
// ASICEN seed regression coverage through the shared PX4 nonce provider.
#include "posix_tuner_nonce_internal.h"

#include <algorithm>
#include <array>
#include <cstdio>

bool run_posix_tuner_nonce_tests();

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc::posix;

#define CHECK(condition)                                                        \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "check failed: %s:%d\n", __FILE__, __LINE__);    \
            return false;                                                      \
        }                                                                      \
    } while (false)

class FailureIo final : public TunerNonceIo {
public:
    Result<int> open_urandom() noexcept override
    {
        return fail_open ? Result<int>::failure(Error::NOT_READY) : Result<int>::success(42);
    }

    Result<EntropyReadResult> read_entropy(
        int descriptor, std::uint8_t* output, std::size_t size) noexcept override
    {
        if (descriptor != 42 || output == nullptr || size == 0U) {
            return Result<EntropyReadResult>::failure(Error::INVALID_ARGUMENT);
        }
        if (reads++ == 0U) {
            const std::size_t count = oversized ? size + 1U : (fail_close ? size : 3U);
            if (count <= size) {
                std::fill_n(output, count, 0xa5U);
            }
            return Result<EntropyReadResult>::success(EntropyReadResult{count, false});
        }
        return Result<EntropyReadResult>::failure(Error::USB_IO);
    }

    Result<void> close_entropy(int descriptor) noexcept override
    {
        if (descriptor != 42) {
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        }
        ++closes;
        return fail_close ? Result<void>::failure(Error::INTERNAL) : Result<void>::success();
    }

    bool fail_open = false;
    bool fail_close = false;
    bool oversized = false;
    std::size_t reads = 0U;
    std::size_t closes = 0U;
};

bool test_failure_does_not_expose_partial_seed()
{
    for (unsigned mode = 0U; mode < 4U; ++mode) {
        FailureIo io;
        io.fail_open = mode == 0U;
        io.fail_close = mode == 1U;
        io.oversized = mode == 2U;
        const auto result = generate_tuner_nonce(io);
        CHECK(!result);
        CHECK(result.value() == (std::array<std::uint8_t, 16U>{}));
        CHECK(io.closes == (io.fail_open ? 0U : 1U));
        const Error expected = mode == 0U ? Error::NOT_READY :
                               mode == 3U ? Error::USB_IO : Error::INTERNAL;
        CHECK(result.error() == expected);
    }
    return true;
}

bool test_native_provider_returns_exact_length()
{
    PosixTunerNonceSource source;
    const auto result = source.generate();
    CHECK(result && result.value().size() == 16U);
    return true;
}

} // namespace

int main()
{
    // The unchanged reference suite checks short/EINTR reads, byte preservation,
    // EOF, read failure, open failure and close failure through the actual provider.
    if (!run_posix_tuner_nonce_tests() || !test_failure_does_not_expose_partial_seed() ||
        !test_native_provider_returns_exact_length()) {
        return 1;
    }
    std::puts("secure entropy tests: PASS");
    return 0;
}
