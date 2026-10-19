# VMAFxScoreErrorBudgetBurn

**Meaning.** Synchronous Score requests (gRPC `Score` and `ScoreStream`,
`POST /v1/score`) on vmafx-server and vmafx-controller fail faster than the
objective (`monitoring.slo.scoreSuccess`, by default 99 % succeed over 30
days) allows. With the default settings: critical above 14.4 % over the last
hour and 5 minutes, warning above 6 % over 6 hours and 30 minutes.
Recorded ratio: `vmafx:score_error_ratio:rate<window>`.

**Impact.** Callers receive errors instead of scores.

## Diagnose

1. Rejections or failures? A full concurrency cap rejects requests with
   `ResourceExhausted` (the server logs `concurrency cap reached`); bad
   requests (missing paths, inputs outside the scoring roots, unknown
   models) are the caller's; scoring failures log `Score failed` with the
   vmaf error.
2. Which instance? Overview _Score requests per second_ by job and instance.
3. Since when? Compare with the _Deploys and restarts_ annotations: a failure
   rate that starts with a rollout points at the new version or its
   configuration.

## Fix

Raise `VMAFX_MAX_CONCURRENT_SCORES` or add server replicas for a full cap;
fix the inputs or the model directory for failures; roll back a bad rollout.

Dashboards: Overview (_Score request errors_, _Score request latency_).
