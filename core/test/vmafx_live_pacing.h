/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The pacing and latency arithmetic of the live-plugin harness
 * (test_vmafx_window_live.c, RC4 WP4, #2238), apart from its clock: the
 * harness passes the monotonic clock, test_vmafx_live_pacing.c a virtual one,
 * so the schedule, the latency and the budget are checked exactly on any host
 * load (maintainer decision Q-325). The wall-clock budget itself is checked
 * by the `timing` suite, which runs alone (docs/development/test-suites.md).
 */

#ifndef VMAFX_LIVE_PACING_H
#define VMAFX_LIVE_PACING_H

#include <stdbool.h>
#include <stdint.h>

#define VL_PERIOD_NS 16666667ull         /* 60 frames per second */
#define VL_BUDGET_NS (2u * VL_PERIOD_NS) /* a window's latency budget */

/* A clock: `now` in nanoseconds, `sleep_until` an absolute deadline on it. */
typedef struct VlClock {
    uint64_t (*now)(void *self);
    void (*sleep_until)(void *self, uint64_t deadline_ns);
    void *self;
} VlClock;

/* Frame i is due at start + i periods. The schedule is absolute: a frame
 * that starts late moves no later frame. */
static inline uint64_t vl_deadline(uint64_t start_ns, uint64_t i)
{
    return start_ns + i * VL_PERIOD_NS;
}

/* Wait for the turn of frame i; returns the time the frame starts, which is
 * its deadline or, when the producer is behind, the time it got there. */
static inline uint64_t vl_pace(const VlClock *clock, uint64_t start_ns, uint64_t i)
{
    clock->sleep_until(clock->self, vl_deadline(start_ns, i));
    return clock->now(clock->self);
}

/* The latency of a window: from the start of the submit of its last frame to
 * the poller seeing it complete; 0 when the completion was seen first. */
static inline uint64_t vl_latency(uint64_t submit_start_ns, uint64_t done_ns)
{
    return done_ns > submit_start_ns ? done_ns - submit_start_ns : 0u;
}

static inline bool vl_within_budget(uint64_t latency_ns)
{
    return latency_ns <= VL_BUDGET_NS;
}

/* The windows the budget applies to: worst latency of the windows cut by the
 * clock ([0]) and of those submitted ahead ([1]), how many were measured and
 * how many were over the budget. */
typedef struct VlLedger {
    uint64_t worst[2];
    unsigned measured;
    unsigned over_budget;
} VlLedger;

static inline void vl_record(VlLedger *ledger, bool ahead, uint64_t latency_ns)
{
    uint64_t *const worst = &ledger->worst[ahead ? 1 : 0];
    *worst = latency_ns > *worst ? latency_ns : *worst;
    ledger->measured++;
    ledger->over_budget += vl_within_budget(latency_ns) ? 0u : 1u;
}

#endif /* VMAFX_LIVE_PACING_H */
