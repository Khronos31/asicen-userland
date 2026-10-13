// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/post_gain_readiness.h"
#include <cstdio>
#include <initializer_list>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main()
{
    using asicen::parse_post_gain_diagnostic;
    using asicen::PostGainReadiness;
    const auto ms = [](int value) { return PostGainReadiness::Time{} + std::chrono::milliseconds(value); };
    CHECK(!parse_post_gain_diagnostic(nullptr).enabled);
    CHECK(parse_post_gain_diagnostic(nullptr).valid);
    CHECK(parse_post_gain_diagnostic(nullptr).stable_ms == 340U);
    CHECK(asicen::PostGainDiagnosticConfig{}.stable_ms == 340U);
    CHECK(parse_post_gain_diagnostic("0").enabled);
    CHECK(parse_post_gain_diagnostic("0").valid);
    CHECK(parse_post_gain_diagnostic("0").stable_ms == 0U);
    CHECK(parse_post_gain_diagnostic("340").enabled);
    CHECK(parse_post_gain_diagnostic("340").valid);
    CHECK(parse_post_gain_diagnostic("340").stable_ms == 340U);
    for (const char* bad : {"", "-1", "1", "340x", " 340", "0340", "999999999999999999"}) {
        CHECK(!parse_post_gain_diagnostic(bad).valid);
    }
    PostGainReadiness gate;
    CHECK(gate.observe(true, ms(0), 0U));
    CHECK(!gate.observe(false, ms(0), 0U));
    CHECK(!gate.observe(true, ms(100), 340U));
    CHECK(!gate.observe(true, ms(439), 340U));
    CHECK(gate.observe(true, ms(440), 340U));
    gate.reset();
    CHECK(!gate.observe(true, ms(100), 340U));
    CHECK(!gate.observe(false, ms(300), 340U));
    CHECK(!gate.observe(true, ms(310), 340U));
    CHECK(!gate.observe(true, ms(440), 340U));
    CHECK(gate.observe(true, ms(650), 340U));
    gate.reset();
    // Prior tune/poll/gain time cannot count toward a newly observed window.
    CHECK(!gate.observe(true, ms(5000), 340U));
    CHECK(!gate.observe(true, ms(5339), 340U));
    CHECK(gate.observe(true, ms(5340), 340U));
    // Completed readiness must not survive release or a new tune.
    gate.reset();
    CHECK(!gate.observe(true, ms(10000), 340U));
    CHECK(!gate.observe(true, ms(10339), 340U));
    CHECK(gate.observe(true, ms(10340), 340U));
    gate.reset();
    CHECK(!gate.observe(true, ms(6000), 340U));
    CHECK(!gate.observe(true, ms(5000), 340U)); // Defensive clock reversal.
    CHECK(!gate.observe(true, ms(5339), 340U));
    CHECK(gate.observe(true, ms(5340), 340U));
    return 0;
}

