// SPDX-License-Identifier: GPL-2.0-only
// Isolated Windows tests for the TS binary sink: byte fidelity including
// LF/CR/NUL/0x1a, broken consumer, stalled-consumer cancellation under a
// bounded deadline, final drain failure, and repeated create/destroy. These
// use anonymous pipes and files only; no USB or network.
#include "px4_ts_windows_output.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::cli;

int failures = 0;

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                         #condition);                                               \
            ++failures;                                                             \
        }                                                                           \
    } while (false)

class NeverStop final : public Px4TsSignal {
public:
    bool stop_requested() const noexcept override { return false; }
};

std::vector<std::uint8_t> make_packet(std::uint8_t seed)
{
    std::vector<std::uint8_t> packet(188U, 0x47U);
    const std::array<std::uint8_t, 8U> special{0x0aU, 0x0dU, 0x00U, 0x1aU,
                                               0x0aU, 0x1aU, 0x0dU, 0x00U};
    for (std::size_t index = 0U; index < packet.size(); ++index) {
        packet[index] = special[index % special.size()];
    }
    packet[0] = seed;
    return packet;
}

std::wstring temp_file_path(const wchar_t* name)
{
    std::array<wchar_t, MAX_PATH> temp{};
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    if (length == 0U) {
        return {};
    }
    std::wstring path(temp.data(), length);
    path += name;
    return path;
}

void test_file_byte_fidelity()
{
    const std::wstring path = temp_file_path(L"px4-ts-output-bytes.bin");
    CHECK(!path.empty());
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0U,
                                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                       nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    {
        auto created = Px4TsWindowsOutput::create(file, true);
        CHECK(created);
        if (created) {
            NeverStop signal;
            const auto packet = make_packet(0x47U);
            for (int index = 0; index < 8; ++index) {
                CHECK(created.value()->write(ByteView{packet.data(), packet.size()},
                                             signal));
            }
            const auto finished = created.value()->finish();
            CHECK(finished);
            CHECK(created.value()->completed_packets() == 8U);
        }
    }
    const HANDLE read = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(read != INVALID_HANDLE_VALUE);
    if (read != INVALID_HANDLE_VALUE) {
        std::vector<std::uint8_t> contents(8U * 188U, 0U);
        DWORD read_bytes = 0U;
        const BOOL ok = ::ReadFile(read, contents.data(),
                                   static_cast<DWORD>(contents.size()), &read_bytes,
                                   nullptr);
        CHECK(ok != 0 && read_bytes == contents.size());
        const auto expected = make_packet(0x47U);
        for (std::size_t packet = 0U; packet < 8U; ++packet) {
            for (std::size_t index = 0U; index < 188U; ++index) {
                CHECK(contents[packet * 188U + index] == expected[index]);
            }
        }
        ::CloseHandle(read);
    }
    (void)::DeleteFileW(path.c_str());
}

void test_broken_consumer()
{
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    CHECK(::CreatePipe(&read_end, &write_end, nullptr, 0) != 0);
    if (write_end == nullptr) {
        return;
    }
    ::CloseHandle(read_end);
    auto created = Px4TsWindowsOutput::create(write_end, true);
    CHECK(created);
    if (!created) {
        return;
    }
    NeverStop signal;
    const auto packet = make_packet(0x01U);
    for (int index = 0; index < 64; ++index) {
        const auto written =
            created.value()->write(ByteView{packet.data(), packet.size()}, signal);
        if (!written) {
            break;
        }
    }
    const auto finished = created.value()->finish();
    CHECK(!finished);
    CHECK(created.value()->final_error() == Error::DISCONNECTED);
}

void test_stalled_consumer_cancellation_is_bounded()
{
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    // Small buffer so the pipe fills quickly and the worker blocks.
    CHECK(::CreatePipe(&read_end, &write_end, nullptr, 4096) != 0);
    if (write_end == nullptr) {
        return;
    }
    auto created = Px4TsWindowsOutput::create(write_end, true);
    CHECK(created);
    if (!created) {
        ::CloseHandle(read_end);
        return;
    }
    // Once the bounded queue is full and the consumer is stalled, a stop
    // signal makes write() return promptly instead of waiting forever.
    class StopSignal final : public Px4TsSignal {
    public:
        bool stop_requested() const noexcept override { return true; }
    } signal;
    const auto packet = make_packet(0x02U);
    for (int index = 0; index < 4096; ++index) {
        const auto written =
            created.value()->write(ByteView{packet.data(), packet.size()}, signal);
        if (!written) {
            break;
        }
    }
    const auto start = std::chrono::steady_clock::now();
    const auto finished = created.value()->finish();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CHECK(!finished);
    CHECK(elapsed.count() < 15000);
    CHECK(created.value()->final_error() != Error::OK);
    ::CloseHandle(read_end);
}

void test_write_after_stop_is_cancellable()
{
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    CHECK(::CreatePipe(&read_end, &write_end, nullptr, 4096) != 0);
    if (write_end == nullptr) {
        return;
    }
    auto created = Px4TsWindowsOutput::create(write_end, true);
    CHECK(created);
    if (!created) {
        ::CloseHandle(read_end);
        return;
    }
    class StopSignal final : public Px4TsSignal {
    public:
        bool stop_requested() const noexcept override { return true; }
    } signal;
    const auto packet = make_packet(0x03U);
    // Fill until the queue is full; a stopped signal must return promptly.
    const auto start = std::chrono::steady_clock::now();
    bool saw_cancellable = false;
    for (int index = 0; index < 4096; ++index) {
        const auto written =
            created.value()->write(ByteView{packet.data(), packet.size()}, signal);
        if (!written && written.error() == Error::TIMEOUT) {
            saw_cancellable = true;
            break;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CHECK(saw_cancellable);
    CHECK(elapsed.count() < 15000);
    (void)created.value()->finish();
    ::CloseHandle(read_end);
}

void test_immediate_finish_exact_bytes()
{
    // Enqueue exactly one packet and finish immediately. If finish() cancelled
    // the in-flight write before it completed, the file would be truncated.
    const std::wstring path = temp_file_path(L"px4-ts-immediate.bin");
    CHECK(!path.empty());
    const auto packet = make_packet(0x5aU);
    for (int iteration = 0; iteration < 200; ++iteration) {
        const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                           0U, nullptr, CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        auto created = Px4TsWindowsOutput::create(file, true);
        CHECK(created);
        if (!created) {
            return;
        }
        NeverStop signal;
        CHECK(created.value()->write(ByteView{packet.data(), packet.size()}, signal));
        const auto finished = created.value()->finish();
        CHECK(finished);
        CHECK(created.value()->completed_packets() == 1U);
        CHECK(created.value()->worker_stopped());
    }
    const HANDLE read = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(read != INVALID_HANDLE_VALUE);
    if (read != INVALID_HANDLE_VALUE) {
        std::array<std::uint8_t, 188U + 1U> contents{};
        DWORD read_bytes = 0U;
        const BOOL ok = ::ReadFile(read, contents.data(),
                                   static_cast<DWORD>(contents.size()), &read_bytes,
                                   nullptr);
        CHECK(ok != 0 && read_bytes == 188U);
        CHECK(std::memcmp(contents.data(), packet.data(), 188U) == 0);
        ::CloseHandle(read);
    }
    (void)::DeleteFileW(path.c_str());
}

void test_repeated_lifecycle()
{
    for (int index = 0; index < 32; ++index) {
        HANDLE read_end = nullptr;
        HANDLE write_end = nullptr;
        CHECK(::CreatePipe(&read_end, &write_end, nullptr, 0) != 0);
        if (write_end == nullptr) {
            return;
        }
        auto created = Px4TsWindowsOutput::create(write_end, true);
        if (!created) {
            ::CloseHandle(read_end);
            CHECK(false);
            return;
        }
        NeverStop signal;
        const auto packet = make_packet(0x04U);
        (void)created.value()->write(ByteView{packet.data(), packet.size()}, signal);
        // Drain reader so the write cannot stall.
        std::array<std::uint8_t, 188U> drained{};
        DWORD read_bytes = 0U;
        (void)::ReadFile(read_end, drained.data(), static_cast<DWORD>(drained.size()),
                         &read_bytes, nullptr);
        CHECK(created.value()->finish());
        ::CloseHandle(read_end);
    }
}

}  // namespace

int main()
{
    test_file_byte_fidelity();
    test_broken_consumer();
    test_stalled_consumer_cancellation_is_bounded();
    test_write_after_stop_is_cancellable();
    test_immediate_finish_exact_bytes();
    test_repeated_lifecycle();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("windows ts output tests: PASS\n");
    return 0;
}

#endif  // _WIN32
