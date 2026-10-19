# VMAFxScoreLatencyBudgetBurn

**Meaning.** Too many synchronous Score requests take longer than the
latency bound (`monitoring.slo.scoreLatencySeconds`, by default 30 seconds):
the default objective (`monitoring.slo.scoreLatency`) is 99 % within the bound
over 30 days. With the default settings: critical when more than 14.4 % of the
requests of the last hour and its last 5 minutes were slower, warning above
6 % over 6 hours and 30 minutes. Recorded ratio:
`vmafx:score_slow_ratio:rate<window>`.

**Impact.** Callers wait longer than they expect; clients with deadlines turn
slow requests into errors.

## Diagnose

1. Are requests queueing behind the concurrency cap? Requests wait for a free
   slot before scoring; Overview _Score request latency_ rising together with
   the request rate points at the cap.
2. Did the work change? Longer or higher-resolution clips take longer: a
   latency rise with a steady rate and no deploy points at the inputs.
3. Is the host saturated? CPU-bound scoring on a host with other load slows
   down; compare the host's CPU use.

## Fix

Add replicas or raise `VMAFX_MAX_CONCURRENT_SCORES` within the host's cores;
send long clips through controller jobs instead of synchronous requests.

Dashboard: Overview (_Score request latency_).
