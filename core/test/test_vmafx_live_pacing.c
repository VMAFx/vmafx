/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The pacing and latency arithmetic of the live-plugin harness
 * (vmafx_live_pacing.h) on a virtual clock: exact, on any host load
 * (maintainer decision Q-325). test_vmafx_window_live runs the same functions
 * on the monotonic clock; its wall-clock budget is checked by the `timing`
 * suite, which runs alone.
 *
 * Holds: frame i is due at start + i periods, also after the producer
 * overran (the schedule is absolute, no drift); a window's latency runs from
 * the submit of its last frame; two frame periods are within the budget, one
 * nanosecond more is not; the ledger keeps the worst latency of each kind of
 * window and counts the windows over the budget. Two planted pacing bugs (a
 * pacer that waits one period from now, and a latency taken from a window's
 * first frame) are refused by the same checks.
 */

#include <stdbool.h>
#include <stdint.h>

#include "test.h"
#include "mu_table.h"
#include "vmafx_live_pacing.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { N_FRAMES = 48, OVERRUN_AFTER = 5, OVERRUN_PERIODS = 3, WINDOW_FRAMES = 12 };

#define START_NS 1000000u /* the virtual clock starts at 1 ms */

/* A virtual clock: `now` moves only when a test advances it or a sleep
 * reaches a later deadline. */
typedef struct Virtual {
    uint64_t now;
} Virtual;

static uint64_t virtual_now(void *self)
{
    return ((const Virtual *)self)->now;
}

static void virtual_sleep_until(void *self, uint64_t deadline_ns)
{
    Virtual *const v = self;
    v->now = deadline_ns > v->now ? deadline_ns : v->now;
}

/* Planted bug: a pacer that waits one period from now instead of until the
 * frame's deadline, so an overrun moves every later frame. */
static uint64_t relative_pace(const VlClock *clock, uint64_t start_ns, uint64_t i)
{
    (void)start_ns;
    const uint64_t now = clock->now(clock->self);
    clock->sleep_until(clock->self, i ? now + VL_PERIOD_NS : now);
    return clock->now(clock->self);
}

typedef uint64_t (*Pacer)(const VlClock *clock, uint64_t start_ns, uint64_t i);

/* Produce N_FRAMES frames with `pace`; the producer overruns by
 * OVERRUN_PERIODS periods after frame OVERRUN_AFTER. Returns the number of
 * frames that did not start at the later of their deadline and the time the
 * producer was ready for them: an absolute schedule starts a frame that is
 * already due at once and every other frame at its deadline. */
static unsigned off_schedule(Pacer pace)
{
    Virtual v = {START_NS};
    const VlClock clock = {virtual_now, virtual_sleep_until, &v};
    unsigned off = 0;
    for (unsigned i = 0; i < N_FRAMES; i++) {
        const uint64_t deadline = vl_deadline(START_NS, i);
        const uint64_t ready = v.now;
        const uint64_t due = deadline > ready ? deadline : ready;
        off += pace(&clock, START_NS, i) != due;
        if (i == OVERRUN_AFTER) {
            v.now += OVERRUN_PERIODS * VL_PERIOD_NS; /* a slow frame */
        }
    }
    return off;
}

static char *test_deadlines_are_absolute(void)
{
    for (unsigned i = 0; i < N_FRAMES; i++) {
        mu_assert("frame i is due at start + i periods",
                  vl_deadline(START_NS, i) == START_NS + (uint64_t)i * VL_PERIOD_NS);
    }
    return NULL;
}

static char *test_pacing_recovers_after_an_overrun(void)
{
    Virtual v = {START_NS};
    const VlClock clock = {virtual_now, virtual_sleep_until, &v};
    for (unsigned i = 0; i <= OVERRUN_AFTER; i++) {
        mu_assert("on schedule before the overrun",
                  vl_pace(&clock, START_NS, i) == vl_deadline(START_NS, i));
    }
    v.now += OVERRUN_PERIODS * VL_PERIOD_NS;
    const uint64_t late = v.now;
    for (unsigned i = OVERRUN_AFTER + 1u; i < OVERRUN_AFTER + OVERRUN_PERIODS; i++) {
        mu_assert("a frame already due starts at once", vl_pace(&clock, START_NS, i) == late);
    }
    const unsigned back = OVERRUN_AFTER + OVERRUN_PERIODS + 1u;
    mu_assert("back on the schedule, not shifted by the overrun",
              vl_pace(&clock, START_NS, back) == vl_deadline(START_NS, back));
    mu_assert("no frame off its schedule", off_schedule(vl_pace) == 0u);
    return NULL;
}

static char *test_planted_relative_pacer_is_refused(void)
{
    mu_assert("a pacer that drifts after an overrun is caught", off_schedule(relative_pace) > 0u);
    return NULL;
}

static char *test_latency_and_budget_boundary(void)
{
    const uint64_t submit = vl_deadline(START_NS, 7);
    mu_assert("latency from the submit of the last frame",
              vl_latency(submit, submit + 5000000u) == 5000000u);
    mu_assert("completion seen before the submit counts as 0",
              vl_latency(submit, submit - 1u) == 0u);
    mu_assert("two frame periods are within the budget", vl_within_budget(VL_BUDGET_NS));
    mu_assert("one nanosecond more is not", !vl_within_budget(VL_BUDGET_NS + 1u));
    mu_assert("the budget is two periods at 60 fps", VL_BUDGET_NS == 33333334ull);
    return NULL;
}

static char *test_ledger_keeps_worst_per_kind(void)
{
    VlLedger ledger = {{0, 0}, 0, 0};
    vl_record(&ledger, false, 10000000u);
    vl_record(&ledger, true, 30000000u);
    vl_record(&ledger, false, 40000000u);
    vl_record(&ledger, false, VL_BUDGET_NS);
    mu_assert("worst of the clock windows", ledger.worst[0] == 40000000u);
    mu_assert("worst of the windows submitted ahead", ledger.worst[1] == 30000000u);
    mu_assert("every window measured", ledger.measured == 4u);
    mu_assert("one window over the budget", ledger.over_budget == 1u);
    return NULL;
}

/* Planted bug: a latency taken from the submit of a window's first frame.
 * A paced window of WINDOW_FRAMES frames that completes 5 ms after its last
 * submit is within the budget; measured from its first frame it is not. */
static char *test_planted_latency_origin_is_refused(void)
{
    const uint64_t first = vl_deadline(START_NS, 0);
    const uint64_t last = vl_deadline(START_NS, WINDOW_FRAMES - 1u);
    const uint64_t done = last + 5000000u;
    mu_assert("measured from the last frame: within", vl_within_budget(vl_latency(last, done)));
    mu_assert("measured from the first frame: refused", !vl_within_budget(vl_latency(first, done)));
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_deadlines_are_absolute),
        MU_TEST(test_pacing_recovers_after_an_overrun),
        MU_TEST(test_planted_relative_pacer_is_refused),
        MU_TEST(test_latency_and_budget_boundary),
        MU_TEST(test_ledger_keeps_worst_per_kind),
        MU_TEST(test_planted_latency_origin_is_refused),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
