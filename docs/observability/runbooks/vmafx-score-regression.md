# VMAFxScoreRegression

**Meaning.** For an hour, the median pooled score of the tenant and model in
the labels has been more than 5 VMAF points below the median of the previous
day's hourly medians, with at least 20 scores in that hour. Recorded series:
`vmafx:quality_score:p50_1h`, `vmafx:quality_score:count_1h`.

**Impact.** The encodes being scored got worse, or the scoring changed. The
platform itself works; the alert is about what it measures.

## Diagnose

1. One tenant or all? On the Quality dashboard, _Median score by tenant_. One
   tenant alone points at its content or its encoder settings.
2. One model or all? _Median score by model_. A drop that starts at a model
   change, or at a deploy (the _Deploys and restarts_ annotation), points at
   the scoring side: a different model version, or a changed default.
3. A shift or a second population? _Score distribution_: the whole band
   moving down is a general change; a second band below is part of the work
   (one encoder ladder rung, one content type).

## Fix

Trace the drop to its source: revert the encoder or ladder change, or confirm
the content changed and the drop is expected. When a model was changed on
purpose the alert clears after a day, when the new level becomes the
baseline.

Dashboard: Quality.
