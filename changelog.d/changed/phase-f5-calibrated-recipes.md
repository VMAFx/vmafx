### Changed — `vmaf-tune auto` Phase F.5 calibrated recipe overrides (ADR-0325)

The F.4 `[provisional, calibrate against real corpus in F.5]`
threshold placeholders in `vmaftune.auto._CONTENT_RECIPE_TABLE` are
now replaced at module-load time with empirically-derived values
from `ai/data/phase_f_recipes_calibrated.json`. The shipped JSON was
emitted by `ai/scripts/calibrate_phase_f_recipes.py` against the
K150K corpus (148 543 of 153 841 expected rows; ingestion was
~96.6 % complete at calibration time and a re-run on the full
corpus is a follow-up PR).

Calibrated values (replaces the F.4 placeholder envelope):

| Class | `tight_interval_max_width` | `force_single_rung` | `saliency_intensity` | `target_vmaf_offset` | Source |
| ----- | -------------------------- | ------------------- | -------------------- | -------------------- | ------ |
| `animation` | `1.75` | `true` | `aggressive` | `+2.0` | proxy (UGC-anchored) |
| `screen_content` | _(unset)_ | _(unset)_ | `very_aggressive` | `+1.0` | proxy (UGC-anchored) |
| `live_action_hdr` | `1.4` | _(unset)_ | _(default)_ | `0.0` | proxy (UGC-anchored) |
| `ugc` | `3.5` | `false` | `default` | `+1.5` | corpus (K150K) |

Honest-data caveats:

- K150K is UGC-only and carries no per-source `content_class`
  column; only the `ugc` row is corpus-derived. The other three
  rows are documented as proxy values anchored on the F.4 envelope
  until PR #477's TransNet shot-metadata columns plus a
  class-labelled subset replace them.
- UGC's empirical `target_vmaf_offset` came out positive (`+1.5`)
  on K150K because the corpus's MOS distribution has a heavier
  upper tail than lower tail. The calibration script clamps every
  offset to the F.4 documented envelope of `[-2.0, +2.0]` so a
  pathological corpus cannot push the predictor target outside the
  regime the planner has been exercised against.
- The `mos_to_vmaf_proxy` mapping (slope 20, intercept 0) is the
  Hosu et al. 2017 §3.3 anchor. A future re-calibration that
  measures end-to-end VMAF against the K150K reference clips will
  replace the proxy with measured scores.

Loader behaviour: `vmaftune.auto._load_calibrated_recipes` walks up
from the module to find `ai/data/phase_f_recipes_calibrated.json`.
If the file is missing, malformed, or missing the `recipes` object,
the F.4 placeholder constants in `_F4_PLACEHOLDER_RECIPES` apply as
a graceful fallback. Per memory `feedback_no_test_weakening`, the
calibration cannot widen the production-flip gate beyond the
ConfidenceThresholds wide-interval ceiling
(`test_calibrated_ugc_width_below_wide_gate_ceiling`).

Reproducer:

```shell
python ai/scripts/calibrate_phase_f_recipes.py \
    --corpus .workingdir2/konvid-150k/konvid_150k.jsonl \
    --out ai/data/phase_f_recipes_calibrated.json
pytest tools/vmaf-tune/tests/test_calibrated_recipes.py \
       tools/vmaf-tune/tests/test_auto_recipe_overrides.py
```

References: ADR-0325 §"Status update 2026-05-09: F.5 calibrated",
Research-0067 §"F.4 recipe-override placeholders", PR #502 (F.4),
PR #477 (TransNet shot-metadata columns — future re-calibration
unlock).
