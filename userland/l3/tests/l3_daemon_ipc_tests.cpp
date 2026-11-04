// SPDX-License-Identifier: GPL-2.0-or-later
#define main asicen_lifecycle_tests_main
#include "libusb_capture_lifecycle_tests.cpp"
#undef main

#include "asicen/device_profile.h"
#include "l3_daemon_bridge.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

void feed_ts(void* context)
{
    auto& fake = *static_cast<FakeUsb*>(context);
    const std::size_t sizes[] = {100U, 600U, 500U, 680U};
    std::size_t offset = 0U;
    for (std::size_t index = 0U; index < fake.submitted.size() && index < 4U; ++index)
    {
        auto* transfer = fake.submitted[index];
        if (transfer == nullptr || transfer->buffer == nullptr)
        {
            continue;
        }
        const std::size_t length = sizes[index];
        for (std::size_t i = 0U; i < length; ++i)
        {
            const std::size_t position = offset + i;
            transfer->buffer[i] =
                position % 188U == 0U ? 0x47U : static_cast<std::uint8_t>(position / 188U & 0x7fU);
        }
        transfer->actual_length = static_cast<int>(length);
        transfer->status = LIBUSB_TRANSFER_COMPLETED;
        transfer->callback(transfer);
        fake.callback_count.fetch_add(1U);
        fake.callback_total.fetch_add(1U);
        offset += length;
    }
}

bool saw_terrestrial_lock_read(const FakeUsb& fake)
{
    for (const auto& event : fake.controls)
    {
        if (event.request == asicen::Request::I2cRead && event.value == 0xb030U)
        {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    FakeUsb fake;
    // W3U3 lock readiness reads demod register 0xb0 and requires low nibble 9.
    fake.terrestrial_b0 = 0xa9U;
    asicen::LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    void* session = asicen_l3_daemon_create(&hardware);
    if (session == nullptr)
    {
        std::fprintf(stderr, "l3 daemon create failed\n");
        return 1;
    }
    asicen_l3_daemon_set_feeder(session, &feed_ts, &fake);
    if (asicen_l3_daemon_start(session) != 0)
    {
        std::fprintf(stderr, "l3 daemon owner start failed\n");
        return 1;
    }

    char runtime_template[] = "/tmp/asicen-l3-daemon-XXXXXX";
    char* runtime_directory = ::mkdtemp(runtime_template);
    if (runtime_directory == nullptr)
    {
        return 2;
    }
    char output_template[] = "/tmp/asicen-l3-daemon-ts-XXXXXX";
    const int output_fd = ::mkstemp(output_template);
    if (output_fd >= 0)
    {
        ::close(output_fd);
    }
    int eof_pipe[2] = {-1, -1};
    if (::pipe(eof_pipe) != 0 || ::dup2(eof_pipe[0], STDIN_FILENO) < 0)
    {
        if (eof_pipe[0] >= 0) ::close(eof_pipe[0]);
        if (eof_pipe[1] >= 0) ::close(eof_pipe[1]);
        return 2;
    }
    ::close(eof_pipe[0]);
    volatile std::sig_atomic_t stop = 0;
    int server_result = 70;
    std::thread server([&] {
        server_result = asicen_l3_daemon_run(session, runtime_directory, "l3-test", &stop);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::thread stdin_eof([&] {
        char byte = 0;
        ssize_t count = -1;
        do
        {
            count = ::read(STDIN_FILENO, &byte, sizeof(byte));
        } while (count < 0 && errno == EINTR);
        stop = 1;
    });

    const pid_t child = ::fork();
    if (child == 0)
    {
        ::close(eof_pipe[1]);
        ::execl(ASICEN_TS_BINARY, "asicen-ts", "--instance", "l3-test", "--runtime-dir",
                runtime_directory, "--receiver", "1", "--channel", "T27", "--packet-count", "10",
                "--output", output_template, static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int child_status = 0;
    if (child > 0)
    {
        (void)::waitpid(child, &child_status, 0);
    }
    ::close(eof_pipe[1]);
    stdin_eof.join();
    server.join();
    asicen_l3_daemon_destroy(session);

    if (child < 0)
    {
        return 3;
    }
    if (!saw_terrestrial_lock_read(fake))
    {
        std::fprintf(stderr, "standard IPC tune did not reach the terrestrial lock-read stage\n");
        return 8;
    }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
    {
        std::fprintf(stderr, "asicen-ts child status=%d\n", child_status);
        return 3;
    }
    if (server_result != 0)
    {
        std::fprintf(stderr, "l3 daemon server result=%d\n", server_result);
        return 4;
    }
    FILE* file = std::fopen(output_template, "rb");
    if (file == nullptr)
    {
        return 5;
    }
    std::size_t total = 0U;
    std::uint8_t buffer[188];
    while (std::fread(buffer, 1U, sizeof(buffer), file) == sizeof(buffer))
    {
        if (buffer[0] != 0x47U)
        {
            std::fclose(file);
            return 6;
        }
        total += sizeof(buffer);
    }
    std::fclose(file);
    if (total != 10U * 188U)
    {
        std::fprintf(stderr, "ts bytes=%zu\n", total);
        return 7;
    }
    ::unlink(output_template);
    return 0;
}
