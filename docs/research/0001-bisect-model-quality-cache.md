# Research-0001: Cache shape for `bisect-model-quality` nightly

- **Status**: Active
- **Workstream**: [ADR-0109](../adr/0109-nightly-bisect-model-quality.md)
- **Last updated**: 2026-04-18

## Question

What does the held-out feature parquet that the nightly
`bisect-model-quality` job consumes need to look like, and can we ship
something useful before the real DMOS-aligned dataset path is in place?

## Sources

- Issue [#4](https://github.com/VMAFx/vmafx/issues/4) — original
  request, prerequisites list, AC.
- [`ai/src/vmaf_train/bisect_model_quality.py`](../../ai/src/vmaf_train/bisect_model_quality.py)
  — algorithm contract (monotonic timeline, threshold gates, log₂(N)
  step bound).
- [`ai/src/vmaf_train/eval.py::evaluate_onnx`](../../ai/src/vmaf_train/eval.py)
  — what the bisect actually feeds to ONNX Runtime: a 2-D feature
  matrix `(N, F)` and a 1-D `mos` target vector.
- [`ai/src/vmaf_train/data/feature_dump.py`](../../ai/src/vmaf_train/data/feature_dump.py)
  — defines `DEFAULT_FEATURES = (adm2, vif_scale0..3, motion2)`. The
  parquet must carry at least one of these column names plus a `mos`
  column.
- [`python/test/resource/yuv/`](../../python/test/resource/yuv/) — the
  ground-truth YUV fixtures we *do* ship; none of them have associated
  DMOS labels.
- `model/tiny/registry.json` — current registry: 2 smoke probes +
  LPIPS-SqueezeNet. No quality timeline yet.

## Findings

- **The bisect tool's data contract is narrow**: a 2-D float32 feature
  matrix + a 1-D float32 target. Anything that satisfies that and
  produces a non-degenerate PLCC/SROCC distribution is a valid input.
  This is what makes a synthetic placeholder viable.
- **256 rows × 6 features (~14 KB parquet) is enough** for stable
  PLCC: the noise floor we add (`N(0, 1e-3)`) keeps PLCC < 1 on
  optimal-weight models while sitting comfortably above the 0.85
  gate. Verified locally: 8 committed models all return PLCC ≈ 0.9999.
- **Byte-stable ONNX serialisation requires pinning** `producer_name`,
  `producer_version`, and `ir_version` on the model proto. Without
  pinning, the bytes drift between `onnx` versions even with identical
  graphs. (Discovered while writing the `--check` regenerator.)
- **Parquet stability requires pinning the row index** with
  `pd.RangeIndex(name="row")` and explicit `compression="zstd"`. Both
  are deterministic across `pyarrow` 17–23 in our smoke runs.
- **Drift detection is the real value-add**: even with a synthetic
  cache, the `--check` step in CI will catch silent serialiser changes
  in pandas / pyarrow / onnx that would otherwise stay invisible until
  the real-cache swap forces a regen.
- **Real DMOS-aligned cache is gated on three independent things**:
  (a) source dataset access (NFLX-public is downloadable; LIVE / KonIQ
  have licensing terms to verify), (b) DMOS labels per clip (NFLX
  publishes alongside the videos; KonIQ ditto; LIVE has subjective
  scores file), (c) a frozen libvmaf build to extract features from
  (so the cache is reproducible across libvmaf versions). All three
  are tractable but none are PR-sized.

## Alternatives explored

- **Use the existing testdata YUVs directly** (`dis_576x324_48f.yuv`
  etc.). Rejected: no DMOS labels, so the `mos` column would have to
  be invented. Same downside as the synthetic placeholder, plus the
  added cost of running libvmaf in CI to dump features. No win.
- **Generate features from libvmaf at workflow runtime**, against the
  existing testdata YUVs, with synthetic MOS = `1 - PSNR_loss`.
  Rejected: needs a libvmaf build step in the bisect workflow,
  multiplying CI time, with no quality signal added because the MOS is
  still synthetic.
- **Bundle the NFLX-public clips in tree under git-lfs**. Deferred:
  blows up clone size; LFS quota considerations; deferred until the
  real-cache swap is the actual blocker.
- **Skip the parquet, dump features inline in the workflow**.
  Rejected: kills determinism (libvmaf updates would change the
  features, decoupling "model regression" from "feature drift").

## Open questions

- Which DMOS-labelled subset is the right first real cache: NFLX-public
  alone (smaller, single-source), or a 3-way mix? Likely NFLX-public
  for the first pass to avoid the licensing matrix.
- What canonical model-timeline ordering will the real swap use:
  registry-listed order, git-log on `model/*.onnx`, or release-tag
  order? Punted — the synthetic timeline uses index 0..7 and the real
  swap will pick when there's a real timeline to order.
- Once a real cache lands, should the synthetic timeline + cache stay
  as a CI smoke (catches wiring breakage) alongside the real one, or
  be retired? Lean toward keeping both: the synthetic one is cheap and
  isolates "wiring broke" from "model regressed".

## Related

- ADRs: [ADR-0109](../adr/0109-nightly-bisect-model-quality.md)
- PRs: this one (closes #4); follow-up TBD for the real cache swap.
- Issues: [#4](https://github.com/VMAFx/vmafx/issues/4),
  [#40](https://github.com/VMAFx/vmafx/issues/40) (sticky tracker).
