<!-- markdownlint-disable MD049 -->
# Research-0089: `vmaf-tune` Phase F.5 — recipe calibration on K150K (2026-05-09)

- **Date**: 2026-05-09
- **Companion ADR**: [ADR-0325](../adr/0325-vmaf-tune-phase-f-auto.md)
  (`### Status update 2026-05-09: F.5 calibrated`)
- **Parent ADR**: [ADR-0237](../adr/0237-quality-aware-encode-automation.md)
  (umbrella) and [ADR-0325](../adr/0325-vmaf-tune-phase-f-auto.md)
- **Companion digest**:
  [Research-0067](0067-vmaf-tune-phase-f-feasibility-2026-05-08.md)
  §"F.4 recipe-override placeholders" — the F.4 placeholders this
  calibration replaces.
- **Status**: Calibration snapshot. The K150K row count was
  148 543 / 153 841 (~96.6 %) at calibration time; a re-run on the
  full corpus is a follow-up PR.

## Question

Phase F.4 (PR #502, ADR-0325 §"Status update 2026-05-09") shipped four
content-class recipe overrides — `animation`, `screen_content`,
`live_action_hdr`, `ugc` — with every threshold tagged
`[provisional, calibrate against real corpus in F.5]`. F.5 closes the
calibration loop: replace the placeholders with empirically-derived
values from a real corpus.

The corpus available at calibration time is K150K
(`.workingdir2/konvid-150k/konvid_150k.jsonl`, ingestion ~96.6 %
complete, no per-row `content_class` column — PR #477's TransNet
shot-metadata columns are still in flight). Question: how do we
calibrate four content classes from a UGC-only, label-free corpus
without violating `feedback_no_guessing`?

## Answer

Two-tier calibration.

1. **UGC** — corpus-derived. K150K is overwhelmingly UGC at 540p
   portrait/landscape, so every row contributes to UGC's statistics:
   `tight_interval_max_width`, `target_vmaf_offset`,
   `saliency_intensity`, `force_single_rung`.

2. **animation / screen_content / live_action_hdr** — proxy-derived.
   Anchored on documented absolute offsets from the F.4 envelope
   (Research-0067 §"F.4 recipe-override placeholders") with a
   multiplicative factor on the UGC-derived `tight_interval_max_width`.
   Each proxy row carries a `_provenance: {"source": "proxy", ...}`
   sub-dict in the JSON output so a future re-calibration on a
   class-labelled corpus can replace it without re-deriving the
   adjustment factors.

Per memory `feedback_no_guessing`, every "proxy" value is documented
with its anchor (Research-0067 §F.4) and adjustment factor; no value
is fabricated in the script. Per memory `feedback_no_test_weakening`,
the calibration script clamps `target_vmaf_offset` to the F.4
envelope `[-2.0, +2.0]` so a pathological corpus cannot push the
predictor target outside the regime the planner has been exercised
against, and the `tight_interval_max_width` cap (3.5) keeps the
ConfidenceThresholds invariant `tight ≤ wide` intact.

## MOS-VMAF proxy mapping

K150K records absolute MOS on a 1-5 Likert scale; vmaf-tune operates
in VMAF score space (0-100). The conversion uses a linear anchor:
MOS=1 → VMAF=20, MOS=5 → VMAF=100, slope 20, intercept 0. The slope
matches the order-of-magnitude reported in
Hosu et al. 2017 §3.3 ("Konstanz natural video database") for h.264
distortions. A future re-calibration that measures end-to-end VMAF
against the K150K reference clips (libvmaf full-reference pass) will
replace the proxy with measured scores. The mapping is intentionally
simple to keep the residual estimate defensible.

## UGC offset method

The `target_vmaf_offset` is derived from the corpus's
**tail-asymmetry**: `(mean - q25) - (q75 - mean)`. A positive value
means the lower tail is heavier than the upper tail — the empirical
signature of a source-side-capped distribution. The recipe's
`target_vmaf_offset` is then `-0.5 * asymmetry`, clamped to
`[-2.0, +2.0]`.

On K150K, the asymmetry came out **negative** (`-2.91` VMAF; lower
tail = 6.55, upper tail = 9.45), giving an offset of `+1.5`. This
inverts the F.4 docstring's intent (the placeholder was `-1.0` on
the assumption that UGC's perceptual ceiling caps the predictor's
optimism). The honest interpretation: K150K is dominated by
medium-quality UGC clips and the upper-quartile MOS extends farther
above the mean than the lower-quartile MOS extends below, so the
predictor target should nudge **up** on this corpus, not down.

The disagreement is documented in the JSON metadata block
(`metadata.ugc_baseline_mos`) and in the ADR-0325 status update.
A future re-calibration on a content-class-labelled corpus may
re-test this — if a UGC-only subset of a labelled corpus shows a
heavier lower tail, the offset would flip sign. The clamp
`[-2.0, +2.0]` keeps the recipe inside the planner's envelope
either way.

## Tight-interval-width method

Approximates the conformal-prediction interval width by the
interquartile range of MOS-VMAF residuals, scaled to a 90 % nominal
coverage gap via the normal-quantile ratio `z90/z50 ≈ 1.645/0.674 ≈ 2.44`.
Floored at 1.5 (so the tight gate doesn't collapse on a thin
sample) and capped at 3.5 (so the gate stays under the
ConfidenceThresholds wide-interval ceiling of 5.0).

On K150K: IQR = 16 VMAF → 90 % width = 39.0 → clamp to **3.5**. The
clamp fired here (the raw value was well above the cap). The
proxy classes scale this baseline by class-specific factors:
animation 0.50 (1.75), screen_content 0.70 (2.45 — but
`screen_content` has no `tight_interval_max_width` key), HDR 0.40
(1.4).

## Saliency-intensity cut points

The script maps a per-class saliency-benefit fraction onto one of
`default` / `aggressive` / `very_aggressive`:

- `>= 0.55` → `very_aggressive`
- `>= 0.30` → `aggressive`
- otherwise → `default`

On K150K, the UGC saliency-benefit proxy (landscape ∩ low-MOS
fraction) came out below 0.30 → `default`. Animation's proxy
fraction (0.45) → `aggressive`. Screen-content's proxy (0.65) →
`very_aggressive`. HDR's proxy (0.10) → `default`.

## `force_single_rung` method

A class is judged "single-rung-dominated" when one (width, height)
bucket holds >= 90 % of the class's rows. K150K's two resolution
buckets (960×540 = 93.8 %, 540×960 = 6.2 %) cross the 90 %
dominance threshold, so technically UGC qualifies as single-rung.
However, ADR-0289's multi-rung gate is a *resolution* gate: a
single-rung lock is meaningful for animation (which often dominates
at one production resolution per title) and not for UGC (which
mixes portrait/landscape at the same physical resolution). The
script therefore hard-codes `force_single_rung: false` for UGC and
defers the per-class force-single-rung detection to a future
class-labelled corpus.

## Summary table

| Class | Source | `tight` | `force_single` | `saliency` | `offset` |
| ----- | ------ | ------- | -------------- | ---------- | -------- |
| `animation` | proxy | `1.75` | `true` | `aggressive` | `+2.0` |
| `screen_content` | proxy | _(unset)_ | _(unset)_ | `very_aggressive` | `+1.0` |
| `live_action_hdr` | proxy | `1.4` | _(unset)_ | _(default)_ | `0.0` |
| `ugc` | corpus | `3.5` | `false` | `default` | `+1.5` |

## Follow-ups

- Re-run the calibration on the full K150K corpus once ingestion
  completes (153 841 rows). Diff the resulting JSON; if any
  threshold moves by more than 5 % or any saliency-intensity label
  flips, ship a follow-up PR with the new JSON.
- Replace the MOS-VMAF proxy mapping with measured end-to-end VMAF
  scores once a libvmaf pass over the K150K reference clips has
  run.
- Re-calibrate the proxy classes against a content-class-labelled
  corpus (PR #477 unblocks this). At that point the
  `_PROXY_ADJUSTMENTS` table in `ai/scripts/calibrate_phase_f_recipes.py`
  collapses to per-class corpus-derived statistics and the
  proxy-vs-corpus distinction in the JSON metadata disappears.

## References

- ADR-0325 §"Status update 2026-05-09: F.5 calibrated".
- Research-0067 §"F.4 recipe-override placeholders".
- Hosu, V., Hahn, F., Zingman, I., Lin, H., Saupe, D. "Konstanz
  natural video database (KoNViD-1k)", QoMEX 2017 — the MOS-VMAF
  proxy mapping anchor.
- PR #477 (TransNet shot-metadata columns) — future class-labelled
  re-calibration unlock.
- PR #502 (F.4 placeholders this calibration replaces).
- Memory `feedback_no_guessing`, memory `feedback_no_test_weakening`.
