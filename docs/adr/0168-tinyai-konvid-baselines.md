# ADR-0168: Tiny-AI Wave 1 baselines C2 + C3 — KoNViD-1k training (T6-1)

- **Status**: Accepted
- **Date**: 2026-04-25
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: tiny-ai, training, onnx, konvid-1k, c2, c3, fork-local

## Context

[BACKLOG T6-1](../../.workingdir2/BACKLOG.md) calls for shipping
"baseline C1/C2/C3 ONNX checkpoints in `model/tiny/`". The
[Wave 1 roadmap](../ai/roadmap.md) defines the three baselines:

- **C1** — `fr_regressor_v1.onnx` (FR; libvmaf features → MOS).
  Target: match or beat `vmaf_v0.6.1` PLCC on Netflix Public.
- **C2** — `nr_metric_v1.onnx` (NR; distorted frame → MOS).
  Target: useful on live-encode + UGC without a reference.
- **C3** — `learned_filter_v1.onnx` (residual; degraded → clean).
  Target: residual luma denoise, ≤ +2% encode time.

The roadmap explicitly notes:

> First training run also exercises the `vmaf-train` CLI end-to-end
> and proves the sidecar-JSON round-trip.

Pre-existing scaffolding under [`ai/`](../../ai/) covered:

- Lightning models for all three (`fr_regressor`, `nr_metric`,
  `learned_filter`), each with hparams matching the YAML configs in
  [`ai/configs/`](../../ai/configs/).
- A C1-shaped `VmafTrainDataModule` that reads
  `(features, mos)` parquet rows, with key-aware splits.
- A typer CLI with `extract-features` / `fit` / `export` /
  `eval` / `register` subcommands.

What was missing: actual ONNX checkpoints (`model/tiny/` had only
LPIPS-SqueezeNet + smoke fixtures), and the data-loading path for
C2 (frame → MOS) / C3 (paired frames) which the C1-only datamodule
could not handle.

A 2026-04-25 dataset-access audit (general-purpose Claude agent)
established that:

- **Netflix Public** (the dataset that calibrates `vmaf_v0.6.1`) is
  access-gated. Distribution is a Google Drive folder requiring a
  manual request to Netflix; **cannot be downloaded
  programmatically**. No public mirror exists. This blocks C1 from
  shipping in this PR.
- **KoNViD-1k** (UGC NR with crowd-sourced MOS) is freely
  downloadable from `datasets.vqa.mmsp-kn.de` with no auth (~2.3 GB
  videos + ~3 MB metadata). Citation required; no Creative Commons
  variant claimed verbatim on the official page despite secondary
  reports.

Per popup 2026-04-25, the user chose: "Defer C1, ship C2 + C3 now."

## Decision

### Ship trained C2 + C3 baselines, defer C1

1. **C2 — `nr_metric_v1`** — train the existing `NRMetric`
   (MobileNet-tiny, ~19K params) on KoNViD-1k middle-frames at
   224×224 grayscale, key-split 80/10/10 train/val/test, 60 epochs
   with early-stopping (patience 15), 16-bit mixed precision on the
   user's RTX 4090. Final `val/mse = 0.382` (MOS scale 1–5; ~RMSE
   0.62 on a tiny-by-tiny model trained on a tiny-by-tiny dataset
   — pipeline is correct, quality is "baseline" per roadmap intent).

2. **C3 — `learned_filter_v1`** — train the existing
   `LearnedFilter` (4-block residual CNN, ~19K params) self-
   supervised: KoNViD-1k middle-frame + synthetic degradation
   (Gaussian σ=1.2 + JPEG-Q35) → clean original. 100 epochs, no
   early stop (loss kept improving), `val/L1 = 0.019` on the 224×224
   normalised luma plane.

3. **Defer C1** — pending Netflix Public Dataset access, tracked in
   [`docs/state.md`](../state.md) under "Open bugs / deferred items".

### Three new dataset-shaped scripts under `ai/scripts/`

- **`fetch_konvid_1k.py`** — `urllib`-based downloader for the
  videos + metadata zips. Ships at the location pointed to by
  `$VMAF_DATA_ROOT/konvid-1k/` (default `~/datasets/konvid-1k/`).
  Idempotent. Note: the `mmsp-kn.de` TLS certificate was observed
  expired 2026-04-25; the script falls back to an unverified SSL
  context for this single hard-coded URL with the CRC + size sanity
  floor as integrity backstop. Comment in the script flags this is
  not a generalisation.
- **`extract_konvid_frames.py`** — drives ffmpeg per-clip to grab
  one luma frame at clip midpoint, resizes to 224×224 grayscale via
  `area` interpolation, writes per-clip `.npy`, builds two parquets
  (C2: `(key, frame_path, mos)`; C3: `(key, deg_path,
  clean_path)`).
- **`train_konvid.py`** — standalone Lightning driver that side-
  steps the C1-only `vmaf_train.train` glue. Imports the existing
  `NRMetric` / `LearnedFilter` Lightning models, plugs them into
  new `FrameMOSDataset` / `PairedFrameDataset` classes, with
  key-split + early-stopping wired in. Two-arg invocation:
  `--model {c2,c3,both}`.
- **`export_tiny_models.py`** — re-uses the existing
  `vmaf_train.models.exports.export_to_onnx` pipeline (opset 17,
  dynamic batch axis, op-allowlist + ORT roundtrip atol 1e-4),
  writes per-model sidecar JSON, patches `model/tiny/registry.json`
  in place.

### Two new in-tree datamodule classes under `vmaf_train.data`

- `FrameMOSDataset` (C2): `(frame[1, H, W], mos[scalar])`.
- `PairedFrameDataset` (C3): `(degraded[1, H, W], clean[1, H, W])`.

Both expose a `.keys` property so the existing `split_keys` helper
gives deterministic per-clip splits (no leakage between train and
val).

### Schema + C-side enum extension for `kind: "filter"`

[`model/tiny/registry.schema.json`](../../model/tiny/registry.schema.json)
gains `"filter"` as a third allowed enum for `kind`. The matching
C-side `VmafModelKind` enum in
[`core/include/libvmaf/model.h`](../../core/include/libvmaf/model.h)
gains `VMAF_MODEL_KIND_DNN_FILTER = 3`, and the sidecar parser in
[`core/src/dnn/model_loader.c`](../../core/src/dnn/model_loader.c)
recognises the new string. Filter models are registry-tracked
(SHA-256-pinned, signed in release) for trust-root hygiene but are
not loaded by the libvmaf scoring path — the ffmpeg `vmaf_pre`
filter consumes them by path.

## Alternatives considered

1. **Stub all three with random-weight ONNX placeholders.**
   Rejected: pipeline-true but not roadmap-true. The whole point of
   T6-1 is to prove the training pipeline, not just the surface.
2. **Wait for Netflix Public access before shipping anything.**
   Rejected: blocking on an asynchronous external approval would
   stall T6-2/3/4/5/6/7 indefinitely. C2 + C3 can ship now.
3. **Substitute KoNViD-1k for C1's training set.** Rejected:
   `vmaf_v0.6.1` PLCC comparison is C1's defining target; using a
   different dataset makes the comparison non-comparable and would
   ship a number we'd have to caveat in the model card.
4. **Train C2 only, defer both C1 and C3.** Rejected: C3 trains
   self-supervised on the same KoNViD frames already extracted for
   C2 — the marginal cost is one additional Lightning trainer and
   60 epochs of training time. C3 is too cheap to defer.
5. **Add a separate `model/tiny/filter_registry.json`.** Rejected:
   maintaining two registries doubles the trust-root surface and
   confuses release tooling. Extending the existing registry's
   enum is the cleaner path.

## Consequences

**Positive:**

- Two real, trained, ONNX-exportable, ORT-validated baseline models
  ship in `model/tiny/`. Closes 2 of the 3 sub-items of T6-1.
- The training pipeline is exercised end-to-end: dataset fetch →
  manifest scan → frame extraction → Lightning training → ONNX
  export → op-allowlist + ORT roundtrip → registry update.
- Future tiny-AI work has working examples to copy from. C1 in
  particular only needs the dataset; the rest of the pipeline is
  proven.
- The `kind: "filter"` enum extension reserves space for future
  pre-/post-processing models (vmaf_post, FastDVDnet) without
  another schema change.

**Negative:**

- C2 quality is baseline-grade (RMSE ~0.62 on 1–5 MOS, well below
  state-of-the-art NR metrics at this size). Improvements should
  use either (a) bigger backbone, (b) more training data, (c)
  multi-frame input. Tracked as future work in
  [`docs/ai/roadmap.md`](../ai/roadmap.md).
- C3 trained on synthetic degradation — real-encoder distortions
  may not match the gaussian + JPEG distribution. Worth re-training
  on real x265 / SVT-AV1 outputs once a paired-encode workflow
  exists.
- KoNViD-1k MOS values are not redistributed: the populated
  manifest stays gitignored and the user must re-run
  `vmaf-train manifest-scan` on a fresh clone. Existing convention
  per [`manifests/README.md`](../../ai/src/vmaf_train/data/manifests/README.md);
  not changing this.
- Cross-backend ULP gate runs against CPU only on the user's box
  (no CUDA EP installed in the venv). The ONNX models are
  deterministic; CUDA EP validation is a follow-up when the
  self-hosted GPU runner from
  [ADR-0167's docs/development/self-hosted-runner.md](0167-doc-drift-enforcement.md)
  is enrolled.
- The C-side enum extension is ABI-additive (new value at the
  end), not breaking, but consumers with `switch` statements that
  don't have `default:` clauses would emit a `-Wswitch` warning.
  Not a problem inside libvmaf (we use `default:`) but flagged in
  rebase-notes.

## Numerical results

### C2 — `nr_metric_v1`

- Dataset: KoNViD-1k middle-frames, 224×224 grayscale, key-split
  80/10/10 (973 train / 106 val / 121 test).
- Architecture: MobileNet-tiny (1×Conv stem + 5×depth-separable
  blocks + AdaptiveAvgPool + Linear), width=16, ~19.1K params.
- Training: AdamW, lr=1e-3, weight_decay=1e-4, 16-bit mixed
  precision, batch=64, 60 epochs with early-stop patience=15.
  Hardware: RTX 4090.
- Final: `val/mse = 0.382` (epoch 23, training stopped here on
  patience). Test set not yet evaluated — left for follow-up.

### C3 — `learned_filter_v1`

- Dataset: same 1200 frames, paired with synthetic `gaussian σ=1.2
  JPEG Q35` degradation.
- Architecture: 4-block residual CNN with `entry: Conv(1→16) +
  4×ResBlock(16) + exit: Conv(16→1)`, ~18.9K params, output clamped
  to [0, 1].
- Training: AdamW, lr=1e-4, 16-bit mixed precision, batch=32, 100
  epochs (no early-stop trigger).
- Final: `val/L1 = 0.019` on the normalised luma plane (~5/255 in
  raw uint8). Visually denoising as expected.

### ONNX export

- Both pass `vmaf_train.op_allowlist.check_graph` (opset 17, no
  forbidden ops).
- Both round-trip through ORT CPU within 1e-4 atol of the PyTorch
  output.
- File sizes: `nr_metric_v1.onnx` ≈ 51 KB; `learned_filter_v1.onnx`
  ≈ 6 KB.

## References

- [BACKLOG T6-1](../../.workingdir2/BACKLOG.md) — backlog row.
- [Wave 1 roadmap](../ai/roadmap.md) — model definitions + targets.
- [ADR-0036](0036-tinyai-wave1-scope-expansion.md) /
  [ADR-0107](0107-tinyai-wave1-scope-expansion.md) — Wave 1 scope.
- [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI docs
  rule.
- [ADR-0166](0166-mcp-server-release-channel.md) — release channel
  for the artifacts (the new ONNX files attach + sign on the next
  release tag via `supply-chain.yml`).
- KoNViD-1k. Hosu, Hahn, Jenadeleh, Lin, Men, Szirányi, Li, Saupe.
  "The Konstanz natural video database (KoNViD-1k)," QoMEX 2017.
  <http://database.mmsp-kn.de>.
- `req` — user popup 2026-04-25: "All three — real training" →
  follow-up: "download them and train locally, we don't have to
  upload the datasets, only the models" → after audit findings:
  "Defer C1, ship C2 + C3 now (Recommended)".
