// SPDX-License-Identifier: GPL-2.0-only
#define main asicen_frontend_entry_for_test
#include "../tools/asicen_frontend.cpp"
#undef main

#include <cstdio>
#include <cstdlib>

namespace {
std::vector<libusb_transfer*> allocated;
unsigned event_calls = 0U;
unsigned free_calls = 0U;
int submit_status = 0;
libusb_transfer* LIBUSB_CALL allocate_transfer(int)
{
    auto* transfer = static_cast<libusb_transfer*>(std::calloc(1U, sizeof(libusb_transfer)));
    allocated.push_back(transfer);
    return transfer;
}
int LIBUSB_CALL submit_transfer(libusb_transfer*)
{
    return submit_status;
}
int LIBUSB_CALL cancel_transfer(libusb_transfer*)
{
    return 0;
}
int LIBUSB_CALL handle_events(libusb_context*, timeval*, int*)
{
    ++event_calls;
    return 0;
}
void LIBUSB_CALL free_transfer(libusb_transfer* transfer)
{
    ++free_calls;
    std::free(transfer);
}
void complete(libusb_transfer* transfer, libusb_transfer_status status, int length)
{
    transfer->status = status;
    transfer->actual_length = length;
    transfer->callback(transfer);
}

bool parse(std::initializer_list<const char*> words, Arguments* result = nullptr)
{
    std::vector<std::string> owned;
    for (const char* word : words)
        owned.emplace_back(word);
    std::vector<char*> pointers;
    for (auto& word : owned)
        pointers.push_back(word.data());
    Arguments local;
    return parse_arguments(static_cast<int>(pointers.size()), pointers.data(),
                           result != nullptr ? result : &local);
}
} // namespace

int main()
{
    unsigned failures = 0U;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    };
    check(parse({"frontend", "--help"}), "help alone");
    check(!parse({"frontend", "-h"}), "no undocumented help alias");
    check(!parse({"frontend", "--help", "--garbage"}), "help cannot bypass validation");
    check(!parse({"frontend", "--device", "1:2", "--device", "1:2", "init"}),
          "duplicate selector rejected");
    check(!parse({"frontend", "--shared-demod", "--shared-demod", "init"}),
          "duplicate boolean rejected");
    check(!parse({"frontend", "--device", "0x1:2", "init"}), "hex selector rejected");
    check(!parse({"frontend", "--device", "+1:2", "init"}), "signed selector rejected");
    check(!parse({"frontend", "--frequency-khz", "0x88000", "tune"}), "hex frequency rejected");
    check(!parse({"frontend", "--frequency-khz", "557142", "init"}), "ignored frequency rejected");
    check(!parse({"frontend", "--output", "unused", "init"}), "ignored output rejected");
    check(!parse({"frontend", "--reg", "10", "init"}), "ignored register rejected");
    check(!parse({"frontend", "--length", "1", "init"}), "ignored length rejected");
    check(!parse({"frontend", "--reset-state", "1", "init"}), "ignored reset rejected");
    check(!parse({"frontend", "--lock-timeout-ms", "100", "lock"}), "unused timeout rejected");
    check(!parse({"frontend", "capture", "--output", "file"}), "duration required");
    check(!parse({"frontend", "capture", "--seconds", "1"}), "output required");
    check(!parse({"frontend", "capture", "--seconds", "31", "--output", "file"}),
          "duration bounded");
    check(!parse({"frontend", "capture", "--seconds", "1", "--output", ""}),
          "empty output rejected");
    Arguments accepted;
    check(parse({"frontend", "--device", "001:029", "--port", "1-2.1", "--reset-state", "1",
                 "capture", "--seconds", "30", "--output", "-"},
                &accepted) &&
              accepted.bus == 1U && accepted.address == 29U && accepted.output == "-",
          "decimal leading zeros and literal output accepted");
    check(!parse_arguments(0, nullptr, nullptr), "invalid argv rejected");
    LibusbQueuedCaptureIo::Operations operations;
    operations.allocate = allocate_transfer;
    operations.submit = submit_transfer;
    operations.cancel = cancel_transfer;
    operations.events = handle_events;
    operations.free = free_transfer;
    {
        LibusbQueuedCaptureIo queue(reinterpret_cast<libusb_context*>(1),
                                    reinterpret_cast<libusb_device_handle*>(1), &operations);
        check(queue.prepare(0x81U, 2U, 188U), "fake queue prepares without USB");
        check(queue.submit(0U) && queue.submit(1U), "ordered submissions");
        asicen::QueueCompletion completion;
        complete(allocated[1U], LIBUSB_TRANSFER_COMPLETED, 188);
        check(!queue.resubmit(1U), "undelivered completion cannot be overwritten");
        check(queue.wait(1U, &completion) == asicen::QueueWait::Timeout,
              "later completion cannot overtake pending head");
        complete(allocated[0U], LIBUSB_TRANSFER_COMPLETED, 188);
        check(queue.wait(1U, &completion) == asicen::QueueWait::Completion &&
                  completion.slot == 0U && completion.size == 188U,
              "head delivered first");
        check(queue.wait(1U, &completion) == asicen::QueueWait::Completion && completion.slot == 1U,
              "later completion delivered second");
        for (const auto status : {LIBUSB_TRANSFER_TIMED_OUT, LIBUSB_TRANSFER_ERROR}) {
            check(queue.resubmit(0U), "resubmit failed completion fixture");
            complete(allocated[0U], status, 100);
            check(queue.wait(1U, &completion) == asicen::QueueWait::Completion &&
                      completion.size == 0U,
                  "timeout and error partial bytes are never published");
        }
        for (const int length : {-1, 189}) {
            check(queue.resubmit(0U), "resubmit invalid length fixture");
            complete(allocated[0U], LIBUSB_TRANSFER_COMPLETED, length);
            check(queue.wait(1U, &completion) == asicen::QueueWait::Error,
                  "negative and oversized completions rejected");
        }
        submit_status = LIBUSB_ERROR_IO;
        check(!queue.submit(0U), "submit error does not create pending ownership");
        submit_status = 0;
        check(queue.submit(0U), "pending drain fixture");
        event_calls = 0U;
        queue.cancel_and_drain();
        check(event_calls == 8U && !queue.drain_complete(),
              "missing callbacks receive exactly eight bounded drain attempts");
        queue.release();
        check(free_calls == 0U, "pending callback owner is not freed");
        complete(allocated[0U], LIBUSB_TRANSFER_CANCELLED, 0);
        queue.release();
        check(free_calls == 2U, "completed callback ownership can be released safely");
    }
    int descriptors[2] = {-1, -1};
    check(::pipe(descriptors) == 0, "create private output pipe");
    if (descriptors[0] >= 0) {
        const int flags = ::fcntl(descriptors[1], F_GETFL, 0);
        check(flags >= 0 && ::fcntl(descriptors[1], F_SETFL, flags | O_NONBLOCK) == 0,
              "nonblocking output pipe");
        std::array<unsigned char, 4096U> bytes{};
        while (::write(descriptors[1], bytes.data(), bytes.size()) > 0) {
        }
        const auto begin = std::chrono::steady_clock::now();
        PosixCaptureOutput output(descriptors[1], begin + std::chrono::milliseconds(20));
        check(!output.write(bytes.data(), 1U), "full pipe stops at finite deadline");
        check(std::chrono::steady_clock::now() - begin < std::chrono::seconds(1),
              "full pipe cannot block indefinitely");
        g_stop = 1;
        check(!output.write(bytes.data(), 1U), "cancelled output refuses writes");
        g_stop = 0;
        (void)::close(descriptors[0]);
        (void)::close(descriptors[1]);
    }
    return failures == 0U ? 0 : 1;
}
