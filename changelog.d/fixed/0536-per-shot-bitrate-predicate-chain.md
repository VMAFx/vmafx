**`vmaf-tune tune-per-shot` — per-shot `bitrate_kbps` now carries real kbps values (ADR-0536)**

PR #1290 (ADR-0531) added the `ShotRecommendation.bitrate_kbps` field and
serialised it as `null` for missing values, but left `_build_per_shot_bisect_predicate`
returning only the predicate closure — the measured `result.bitrate_kbps` from the
bisect was discarded before return. All shots in any per-shot plan JSON showed
`bitrate_kbps: null`.

The fix introduces a `bitrate_sidecar` dict returned alongside the predicate.
The closure populates the sidecar (keyed by `(start_frame, end_frame)`) as each
shot's bisect completes; `_run_tune_per_shot` patches each `ShotRecommendation`
via `dataclasses.replace` after `tune_per_shot` returns. The `PredicateFn` public
type alias is unchanged — no blast radius on `--predicate-module` callers.
