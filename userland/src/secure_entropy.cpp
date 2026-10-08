// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/secure_entropy.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/random.h>
#endif

namespace asicen {
namespace {

class NativeSecureEntropyIo final : public SecureEntropyIo {
public:
    bool open() noexcept override {
#if defined(__linux__) && !defined(__ANDROID__)
        return true;
#else
        descriptor_ = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        return descriptor_ >= 0;
#endif
    }

    EntropyRead read(std::uint8_t* output, std::size_t size) noexcept override {
#if defined(__linux__) && !defined(__ANDROID__)
        // Keep Linux's existing getrandom(..., 0) source and flags unchanged.
        const ssize_t result = ::getrandom(output, size, 0);
#else
        const ssize_t result = ::read(descriptor_, output, size);
#endif
        if (result < 0 && errno == EINTR) {
            return {EntropyReadStatus::Interrupted, 0U};
        }
        if (result <= 0) return {EntropyReadStatus::Error, 0U};
        return {EntropyReadStatus::Data, static_cast<std::size_t>(result)};
    }

    bool close() noexcept override {
#if defined(__linux__) && !defined(__ANDROID__)
        return true;
#else
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor >= 0 && ::close(descriptor) == 0;
#endif
    }

private:
#if !defined(__linux__) || defined(__ANDROID__)
    int descriptor_ = -1;
#endif
};

}  // namespace

bool fill_secure_entropy_with_io(SecureEntropyIo& io, std::uint8_t* output,
                                 std::size_t size) noexcept {
    if (output == nullptr || size == 0U) return false;
    std::size_t offset = 0U;
    bool success = io.open();
    if (success) {
        while (offset < size) {
            const EntropyRead result = io.read(output + offset, size - offset);
            if (result.status == EntropyReadStatus::Interrupted) continue;
            if (result.status != EntropyReadStatus::Data || result.bytes == 0U ||
                result.bytes > size - offset) {
                success = false;
                break;
            }
            offset += result.bytes;
        }
        if (!io.close()) success = false;
    }
    if (!success || offset != size) {
        for (std::size_t i = 0U; i < size; ++i) output[i] = 0U;
        return false;
    }
    return true;
}

bool fill_secure_entropy(std::uint8_t* output, std::size_t size) noexcept {
    NativeSecureEntropyIo io;
    return fill_secure_entropy_with_io(io, output, size);
}

}  // namespace asicen
