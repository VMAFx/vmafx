# Research Digest: Signal-Mix Audit

## Question

How can the fork quickly identify quality dimensions where the current AI and
vmaf-tune tables have no metric/signal coverage, and where not-yet-wired
metrics could add value through intersection with existing signals?

## Current Inputs

Existing audits and scripts cover adjacent ground:

- `ai/scripts/feature_correlation.py` ranks feature columns against a target
  and finds redundant numeric pairs, but it expects one parquet and has no
  concept of HDR, saliency, codec, or panel signal families.
- `docs/research/feature-coverage-audit-2026-05-18.md` catalogued model and
  extractor coverage at that date. Some script-level findings are now stale
  because the full-feature extraction defaults were widened afterward.
- `docs/research/continuous-feature-mix-evaluation-design-2026-05-18.md`
  sketches the larger evaluator needed for model-grid promotion, but it is
  deliberately bigger than a fast audit.

The missing piece is a current table reader that turns generated data into a
human-readable signal map.

## Findings

- A purely model-centric audit is too late. If the table lacks saliency, panel,
  HDR mastering, or texture columns, no retraining loop can recover that signal.
- A purely extractor-centric audit is also insufficient. A feature can exist in
  libvmaf and still be absent, all-NaN, or flat in the refreshed training table.
- Correlation alone hides coverage gaps. Two columns can rank well against VMAF
  but belong to the same family, while an unrepresented family such as panel
  metadata or no-reference UGC can be the real blocker.
- Candidate metrics need to be visible before they are implemented. Operators
  should see gaps such as DISTS/LPIPS texture, HDR-VDP/PU metrics, DOVER/Q-Align
  MOS, U2NetP ROI, and panel metadata in the report even when the current table
  has no such columns.

## Implementation Shape

`ai/scripts/signal_mix_audit.py` implements a table-only diagnostic pass:

1. Read parquet, JSONL/NDJSON, or JSON row-list inputs.
2. Detect a target column (`vmaf`, `vmaf_score`, `mos_raw_0_100`, `mos`,
   `dmos`, or `score`) unless `--target` is provided.
3. Classify columns into signal families using conservative regexes.
4. Mark families as `covered`, `weak`, or `missing`.
5. Compute per-column Pearson/Spearman correlations against the target when
   present.
6. Report redundant pairs and cross-family complementary intersections.
7. Render JSON for machines and Markdown for humans.

The signal families are intentionally user-facing: canonical FR, PSNR/HVS
energy, SSIM, texture/deep perceptual, color/chroma, banding/tone-mapping,
temporal/scene, saliency/ROI, HDR display/panel, source geometry/content
metadata, codec/rate control, NR/MOS, and noise/grain/blur.

## Validation

The unit tests use synthetic JSONL rows so the audit can run without libvmaf,
pyarrow, or corpus data:

```bash
.venv/bin/python -m pytest ai/tests/test_signal_mix_audit.py -q
```

The tests cover:

- signal-family coverage and missing rows;
- weak HDR/panel coverage when bit-depth is present but flat;
- redundant near-duplicate canonical columns;
- cross-family complementary intersections;
- Markdown rendering of blind spots and candidate metrics;
- CLI JSON/Markdown output.

## Limitations

- Family classification is name-based. New table schemas with unusual column
  names require regex updates.
- Complementary intersections use linear correlation heuristics. They do not
  replace SHAP, permutation importance, LOSO retraining, or the continuous
  feature-mix evaluator.
- The audit cannot infer a metric that was never extracted; it can only mark the
  family missing and list useful candidates.

## Recommendation

Ship the diagnostic as an advisory CLI now, run it on every refreshed table
before model promotion, and use its blind-spot rows to choose concrete follow-up
PRs: panel metadata, saliency/U2NetP, texture metrics, no-reference UGC/MOS, or
codec-specific profile features.
