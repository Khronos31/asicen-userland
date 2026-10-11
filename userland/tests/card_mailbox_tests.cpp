#include "asicen/card_mailbox.h"

#include <iostream>

namespace {
bool check(bool value, const char* name)
{
    if (!value) {
        std::cerr << "FAIL: " << name << '\n';
        return false;
    }
    return true;
}

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!check(__VA_ARGS__)) {                                                                 \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

}  // namespace

bool run_tests()
{
    CHECK(asicen::build_card_mailbox_chunks(0).empty(), "zero length");
    const auto largest = asicen::build_card_mailbox_chunks(64U * 256U);
    CHECK(largest.size() == 256U && largest.back().page == 255U,
          "raw mailbox final representable page");
    CHECK(asicen::build_card_mailbox_chunks(64U * 256U + 1U).empty(),
          "raw mailbox page overflow rejected");
    CHECK(asicen::build_card_mailbox_chunks(static_cast<std::size_t>(-1)).empty(),
          "raw mailbox size overflow rejected before allocation");

    const auto small = asicen::build_card_mailbox_chunks(5);
    CHECK(small.size() == 1, "small chunk count");
    if (small.size() == 1) {
        CHECK(small[0].page == 0 && small[0].register_address == 0x40 &&
                  small[0].buffer_offset == 0 && small[0].length == 5,
              "small chunk fields");
    }

    const auto exact = asicen::build_card_mailbox_chunks(64);
    CHECK(exact.size() == 1 && exact[0].length == 64, "exact page");

    const auto split = asicen::build_card_mailbox_chunks(130);
    CHECK(split.size() == 3, "multi page count");
    if (split.size() == 3) {
        CHECK(split[0].page == 0 && split[0].register_address == 0x40 && split[0].length == 64,
              "page0");
        CHECK(split[1].page == 1 && split[1].register_address == 0x40 &&
                  split[1].buffer_offset == 64 && split[1].length == 64,
              "page1");
        CHECK(split[2].page == 2 && split[2].register_address == 0x40 &&
                  split[2].buffer_offset == 128 && split[2].length == 2,
              "page2");
    }

    CHECK(asicen::decode_card_mailbox_length(0x23, 0) == 0x23, "length low");
    CHECK(asicen::decode_card_mailbox_length(0x23, 1) == 0x123, "length high bit");
    CHECK(asicen::decode_card_mailbox_length(0x23, 3) == 0x123, "length masks high source");

    const auto boundaries = [](std::size_t length, std::size_t chunks) {
        const auto plan = asicen::build_card_mailbox_io_chunks(length);
        if (plan.size() != chunks) {
            return false;
        }
        std::size_t covered = 0U;
        for (const auto& item : plan) {
            if (item.length == 0U || item.length > 8U || item.buffer_offset != covered ||
                item.register_address < asicen::CardMailboxFacts::kDataWindowBase ||
                static_cast<std::size_t>(item.register_address) + item.length > 0x80U) {
                return false;
            }
            covered += item.length;
        }
        return covered == length;
    };
    CHECK(boundaries(8U, 1U), "8-byte window transfer");
    CHECK(boundaries(9U, 2U), "9-byte window split");
    CHECK(boundaries(63U, 8U), "63-byte page tail");
    CHECK(boundaries(64U, 8U), "64-byte page exact");
    CHECK(boundaries(65U, 9U), "65-byte page crossing");
    CHECK(boundaries(255U, 32U), "255-byte card frame window plan");
    CHECK(boundaries(256U, 32U), "256-byte logical mailbox window plan");
    CHECK(boundaries(511U, 64U), "511-byte maximum mailbox window plan");
    CHECK(asicen::build_card_mailbox_io_chunks(512U).empty(), "512-byte mailbox length rejected");

    return true;
}

int main()
{
    return run_tests() ? 0 : 1;
}
