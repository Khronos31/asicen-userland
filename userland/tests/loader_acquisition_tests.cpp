// SPDX-License-Identifier: GPL-2.0-only
#include "loader_acquisition.h"

#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <vector>

namespace {
namespace acquisition = asicen::cli::loader_acquisition;
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
using Attempt = acquisition::Attempt;
using Result = acquisition::Result;

// The resolver predicate is typed and does not depend on enum numeric values.
enum class ResolveError { OK = 0, NOT_FOUND = 41, NOT_READY = 73, OTHER = 99 };

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (false)

// Only acquisition is simulated. No firmware, GPIO, or real USB operation is
// performed, so passing these tests is not hardware/re-enumeration evidence.
struct Step {
    Attempt outcome;
    Milliseconds elapsed{0};
    bool stop_after = false;
};

struct Fixture {
    Clock::time_point time{};
    bool stopped = false;
    bool stop_during_wait = false;
    std::vector<Step> steps;
    std::vector<Clock::duration> waits;
    std::vector<Clock::time_point> starts;
    std::size_t calls = 0;

    explicit Fixture(std::initializer_list<Step> script) : steps(script) {}

    Result run(bool retry = true)
    {
        return acquisition::run(
            retry, [this] { return time; }, [this] { return stopped; },
            [this] {
                starts.push_back(time);
                const Step step = calls < steps.size()
                    ? steps[calls] : Step{Attempt::Retry};
                ++calls;
                time += step.elapsed;
                stopped = stopped || step.stop_after;
                return step.outcome;
            },
            [this](Clock::duration duration) {
                waits.push_back(duration);
                time += duration;
                stopped = stopped || stop_during_wait;
            });
    }
};

bool old_one_shot_fails_new_acquisition_recovers()
{
    Fixture old_path{{Attempt::Retry}, {Attempt::Acquired}};
    CHECK(old_path.run(false) == Result::Failed);
    CHECK(old_path.calls == 1);
    CHECK(old_path.waits.empty());

    Fixture new_path{{Attempt::Retry}, {Attempt::Acquired}};
    CHECK(new_path.run() == Result::Acquired);
    CHECK(new_path.calls == 2);
    CHECK(new_path.waits.size() == 1);
    CHECK(new_path.waits[0] == Milliseconds(250));
    CHECK(new_path.starts[1] == Clock::time_point{} + Milliseconds(250));
    return true;
}

bool transient_classification_is_narrow()
{
    for (const int error : {-1, -4, -5}) {
        CHECK(acquisition::retryable_open_error(error));
    }
    // Includes INVALID_PARAM, ACCESS, BUSY, TIMEOUT, OVERFLOW, PIPE,
    // INTERRUPTED, NO_MEM, NOT_SUPPORTED and OTHER, in libusb numeric form.
    for (const int error : {0, -2, -3, -6, -7, -8, -9, -10, -11, -12, -99}) {
        CHECK(!acquisition::retryable_open_error(error));
        Fixture fixture{{acquisition::retryable_open_error(error)
                             ? Attempt::Retry : Attempt::Failed},
                        {Attempt::Acquired}};
        CHECK(fixture.run() == Result::Failed);
        CHECK(fixture.calls == 1);
        CHECK(fixture.waits.empty());
    }
    CHECK(acquisition::retryable_resolve_error(ResolveError::NOT_FOUND));
    CHECK(acquisition::retryable_resolve_error(ResolveError::NOT_READY));
    CHECK(!acquisition::retryable_resolve_error(ResolveError::OK));
    CHECK(!acquisition::retryable_resolve_error(ResolveError::OTHER));
    return true;
}

bool fresh_resolution_recovers_old_generation_open_failure()
{
    for (const int open_error : {-1, -4, -5}) {
        for (const bool retry : {false, true}) {
            Clock::time_point now{};
            unsigned resolutions = 0;
            unsigned opens = 0;
            unsigned waits = 0;
            unsigned opened_generation = 0;
            const auto result = acquisition::run(
                retry, [&] { return now; }, [] { return false; },
                [&] {
                    // Each attempt resolves anew, rather than reopening the
                    // device reference retained from an earlier generation.
                    const unsigned resolved_generation = ++resolutions;
                    ++opens;
                    if (resolved_generation == 1) {
                        return acquisition::retryable_open_error(open_error)
                            ? Attempt::Retry : Attempt::Failed;
                    }
                    opened_generation = resolved_generation;
                    return Attempt::Acquired;
                },
                [&](Clock::duration delay) { ++waits; now += delay; });
            CHECK(result == (retry ? Result::Acquired : Result::Failed));
            CHECK(resolutions == (retry ? 2U : 1U));
            CHECK(opens == resolutions);
            CHECK(waits == (retry ? 1U : 0U));
            CHECK(opened_generation == (retry ? 2U : 0U));
        }
    }
    return true;
}

bool resolution_transients_do_not_open_until_resolved()
{
    Clock::time_point now{};
    const ResolveError script[] = {ResolveError::NOT_FOUND,
                                   ResolveError::NOT_READY, ResolveError::OK};
    unsigned resolutions = 0;
    unsigned opens = 0;
    unsigned waits = 0;
    const auto result = acquisition::run(
        true, [&] { return now; }, [] { return false; },
        [&] {
            const ResolveError error = script[resolutions < 3 ? resolutions : 2];
            ++resolutions;
            if (error != ResolveError::OK) {
                return acquisition::retryable_resolve_error(error)
                    ? Attempt::Retry : Attempt::Failed;
            }
            ++opens;
            return Attempt::Acquired;
        }, [&](Clock::duration delay) { ++waits; now += delay; });
    CHECK(result == Result::Acquired);
    CHECK(resolutions == 3);
    CHECK(opens == 1);
    CHECK(waits == 2);
    CHECK(now == Clock::time_point{} + Milliseconds(500));
    return true;
}

bool immediate_success_has_no_extra_work()
{
    for (const bool retry : {false, true}) {
        Fixture fixture{{Attempt::Acquired}};
        CHECK(fixture.run(retry) == Result::Acquired);
        CHECK(fixture.calls == 1);
        CHECK(fixture.waits.empty());
        CHECK(fixture.time == Clock::time_point{});
    }
    return true;
}

bool permanent_failure_is_immediate()
{
    for (const bool retry : {false, true}) {
        Fixture fixture{{Attempt::Failed}, {Attempt::Acquired}};
        CHECK(fixture.run(retry) == Result::Failed);
        CHECK(fixture.calls == 1);
        CHECK(fixture.waits.empty());
    }
    Fixture after_retry{{Attempt::Retry}, {Attempt::Failed}, {Attempt::Acquired}};
    CHECK(after_retry.run() == Result::Failed);
    CHECK(after_retry.calls == 2);
    CHECK(after_retry.waits.size() == 1);
    return true;
}

bool repeated_failure_uses_one_deadline()
{
    Fixture fixture{};
    CHECK(fixture.run() == Result::TimedOut);
    CHECK(fixture.calls == 40);
    CHECK(fixture.waits.size() == 40);
    CHECK(fixture.time == Clock::time_point{} + Milliseconds(10000));
    for (const auto duration : fixture.waits) CHECK(duration == Milliseconds(250));
    for (std::size_t index = 0; index < fixture.starts.size(); ++index) {
        CHECK(fixture.starts[index] == Clock::time_point{} +
              Milliseconds(static_cast<Milliseconds::rep>(index) * 250));
    }
    return true;
}

bool operation_time_counts_and_final_wait_is_clamped()
{
    Fixture fixture{{Attempt::Retry, Milliseconds(9800)}, {Attempt::Acquired}};
    CHECK(fixture.run() == Result::TimedOut);
    CHECK(fixture.calls == 1);
    CHECK(fixture.waits.size() == 1);
    CHECK(fixture.waits[0] == Milliseconds(200));
    CHECK(fixture.time == Clock::time_point{} + Milliseconds(10000));

    Fixture overrun{{Attempt::Retry, Milliseconds(10001)}, {Attempt::Acquired}};
    CHECK(overrun.run() == Result::TimedOut);
    CHECK(overrun.calls == 1);
    CHECK(overrun.waits.empty());
    return true;
}

bool cancellation_prevents_more_acquisition()
{
    Fixture before{{Attempt::Acquired}};
    before.stopped = true;
    CHECK(before.run() == Result::Cancelled);
    CHECK(before.calls == 0);
    CHECK(before.waits.empty());

    Fixture during_attempt{{Attempt::Retry, Milliseconds(1), true}, {Attempt::Acquired}};
    CHECK(during_attempt.run() == Result::Cancelled);
    CHECK(during_attempt.calls == 1);
    CHECK(during_attempt.waits.empty());

    Fixture during_wait{{Attempt::Retry}, {Attempt::Acquired}};
    during_wait.stop_during_wait = true;
    CHECK(during_wait.run() == Result::Cancelled);
    CHECK(during_wait.calls == 1);
    CHECK(during_wait.waits.size() == 1);
    return true;
}

bool started_success_wins_even_when_operation_returns_late()
{
    // The deadline bounds starting another attempt, not a synchronous open.
    for (const auto duration : {Milliseconds(10000), Milliseconds(10001)}) {
        Fixture fixture{{Attempt::Acquired, duration}};
        CHECK(fixture.run() == Result::Acquired);
        CHECK(fixture.calls == 1);
        CHECK(fixture.waits.empty());
    }
    Fixture last_attempt{{Attempt::Retry, Milliseconds(9500)},
                         {Attempt::Acquired, Milliseconds(500)}};
    CHECK(last_attempt.run() == Result::Acquired);
    CHECK(last_attempt.calls == 2);
    CHECK(last_attempt.starts[1] == Clock::time_point{} + Milliseconds(9750));
    CHECK(last_attempt.time == Clock::time_point{} + Milliseconds(10250));
    return true;
}

bool repeated_transients_do_not_restart_deadline()
{
    Fixture fixture{{Attempt::Retry, Milliseconds(4000)},
                    {Attempt::Retry, Milliseconds(4000)},
                    {Attempt::Retry, Milliseconds(1400)},
                    {Attempt::Acquired}};
    CHECK(fixture.run() == Result::TimedOut);
    CHECK(fixture.calls == 3);
    CHECK(fixture.waits.size() == 3);
    CHECK(fixture.waits[0] == Milliseconds(250));
    CHECK(fixture.waits[1] == Milliseconds(250));
    CHECK(fixture.waits[2] == Milliseconds(100));
    CHECK(fixture.time == Clock::time_point{} + Milliseconds(10000));
    return true;
}
} // namespace

int main()
{
    if (!old_one_shot_fails_new_acquisition_recovers() ||
        !transient_classification_is_narrow() ||
        !fresh_resolution_recovers_old_generation_open_failure() ||
        !resolution_transients_do_not_open_until_resolved() ||
        !immediate_success_has_no_extra_work() ||
        !permanent_failure_is_immediate() ||
        !repeated_failure_uses_one_deadline() ||
        !operation_time_counts_and_final_wait_is_clamped() ||
        !cancellation_prevents_more_acquisition() ||
        !started_success_wins_even_when_operation_returns_late() ||
        !repeated_transients_do_not_restart_deadline()) return 1;
    std::puts("loader acquisition tests passed (offline policy only)");
    return 0;
}
