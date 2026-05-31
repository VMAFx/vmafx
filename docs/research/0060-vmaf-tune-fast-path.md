<!-- markdownlint-disable MD060 -->
# Research-0060: `vmaf-tune fast` — proxy-based recommend (Phase A.5)

- **Date**: 2026-05-03
- **Companion ADR**: [ADR-0276](../adr/0276-vmaf-tune-fast-path.md)
- **Parent ADR**: [ADR-0237](../adr/0237-quality-aware-encode-automation.md)
  (umbrella, Phase A Accepted)
- **Status**: Snapshot at proposal time. The production fast-path
  implementation PR will supersede the operational details; this
  digest stays as the "why proxy + Bayesian + GPU verify" reference.

## Question

Phase A (PR #329) ships a grid-sweep corpus generator: for every
`(preset, crf)` cell, encode with `libx264`, score with `libvmaf`,
emit a JSONL row. On 1080p sources at `medium` preset that loop
costs roughly 5–15 seconds per cell on a workstation, scaling
linearly with grid size. A typical corpus run for one source over
`(medium, slow) × CRF 18..40` is ≈140 cells ≈ 30–60 minutes;
expanding to four presets and the full CRF range pushes it past
two hours per source.

The user's framing — "vmaf-tune planning should be GPU/AI fast —
Netflix-quality decisions in short time" — asks whether the fork's
existing AI primitives can collapse that wall-time without
sacrificing the recommendation quality the slow grid would have
produced. The slow grid stays as ground truth (ADR-0237 contract);
the question is whether a thin, opt-in `fast` subcommand can hit
within a small VMAF tolerance of the grid's optimum in
seconds-to-minutes rather than hours.

## Bottleneck analysis (Phase A wall-time)

A single Phase A grid cell decomposes into three serial stages.
Wall-time profile from a 1080p 10-second clip on x86-64 (12-core
workstation, no GPU usage):

| Stage | Tool | Wall-time | Share |
|---|---|---|---|
| 1. Encode | `ffmpeg -c:v libx264 -preset medium -crf N` | 3–9 s | 60–80 % |
| 2. Decode + score | `vmaf` (CPU bit-exact, full feature set) | 1.5–3 s | 15–30 % |
| 3. JSONL emit + cleanup | `corpus.py` | < 0.1 s | < 2 % |

Three observations drive the fast-path design:

1. **The encode step dominates.** Slower presets (`slow`, `veryslow`)
   shift even more of the budget into stage 1 — `veryslow` on 1080p
   is closer to 60+ seconds per cell. The grid scales as
   `|presets| × |CRF range| × #sources`; on a 5-source × 4-preset ×
   23-CRF sweep that's 460 cells × ~10 s ≈ 75 minutes minimum.
2. **Stage 2 is independently a known cost.** CPU bit-exact VMAF on
   1080p runs at roughly 1–2 fps; the fork's CUDA / Vulkan / SYCL
   backends already accelerate it 8–20× (ADR-0157 / ADR-0186 / the
   Phase 5 GPU work) but Phase A's harness invokes the CPU CLI
   path because the codepath is the most reproducible.
3. **The grid is unstructured.** Every cell is encoded + scored
   independently; there is no early termination, no surrogate
   model, no Bayesian prior. The corpus is exhaustive by design
   (Phase A's purpose is to be the training set Phase B/C consume),
   but the *recommendation* use case — "pick the CRF that hits
   target VMAF with minimum bitrate" — does not need exhaustive
   coverage.

## Acceleration levers

The fork already ships every primitive a fast-path needs. We
classify them by where in the grid loop they intervene:

### Lever A — VMAF proxy via `fr_regressor_v2` (codec-aware)

Skip stage 2 entirely for grid exploration. `fr_regressor_v1`
(257 params, canonical-6 → VMAF, ADR-0249, ADR-0235) and the
in-flight `fr_regressor_v2` (codec-aware, 9-D conditioning over
encoder × preset × CRF, ADR-0272 / Research-0058) are MLP models
designed for exactly this prediction shape: estimate VMAF without
running VMAF. Inference is microseconds per row on CPU; a 460-cell
"grid" becomes a sub-second sweep.

The proxy's job is **not** to replace `libvmaf` — it is to rank
candidates well enough that the search converges on the correct
neighbourhood, after which a single full VMAF measurement
verifies the chosen point. Calibration cost: Pearson PLCC against
real VMAF on the parent's training corpus is the gating metric;
v1 reports 0.9+ on Netflix Public, v2 adds the codec one-hot to
handle the `medium` vs `slow` rate-distortion shift.

**Speedup ceiling on its own**: ≈50× (entire stage 2 collapses
across the grid; stage 1 is unchanged).

### Lever B — Bayesian search via Optuna TPE

Replace the dense grid with a sparse Bayesian sample. Optuna's
TPE sampler typically reaches the same final VMAF in
≈1/10 the trials a dense grid needs, because each trial's posterior
narrows the search range. CRF is a one-dimensional ordinal in
`[10, 51]` for x264 — the easiest possible BO surface.

**Speedup on its own**: 5–10× on trial count. Combines
multiplicatively with lever A: Bayesian sampling over a proxy is
both fewer trials *and* cheaper per trial.

### Lever C — Hardware-encoded grid via NVENC / QSV / AMF

Replace `libx264` with NVENC (`h264_nvenc`) for the encode step.
Hardware-encoded H.264 on a desktop GPU runs at hundreds of fps
(realtime ≈ 30 fps, NVENC ≈ 300–600 fps on Ada-class) — 1–2 orders
of magnitude faster than `libx264 medium` software. The cost is
that NVENC's rate-distortion curve differs from `libx264`: same
CRF lands at different VMAF / bitrate. The fr_regressor_v2's
encoder one-hot is the right shape to handle this — *if* the
training corpus covered NVENC, which Phase A's `libx264`-only
corpus does not yet.

**Speedup ceiling on its own**: 10–30×, gated on encoder
availability and a follow-up calibration corpus.

### Lever D — Per-shot parallelisation via TransNet V2

Split the source into shots (TransNet V2 real weights via PR #334)
and run an independent recommendation per shot, then aggregate.
This is orthogonal to A/B/C: it makes the optimisation embarrassingly
parallel along the time axis. Wins compound on long sources where a
single CRF is suboptimal anyway. Out of scope for the Phase A.5
scaffold; queued as a Phase D follow-up under the existing
`vmaf-perShot` story (ADR-0222).

### Lever E — GPU VMAF backend for the verification pass

When the proxy converges, run **one** real VMAF measurement at the
recommended CRF to verify the prediction. Use CUDA / Vulkan / SYCL
(ADR-0157 / ADR-0186) for an 8–20× speedup on the verify step. The
verify is one cell, not the grid, so the GPU win matters less than
the proxy win — but it closes the loop with a real measurement and
costs single-digit seconds.

**Speedup contribution**: small in absolute wall-time (one
measurement), large in *trust* (the recommended CRF is grounded in
a real number, not just a proxy estimate).

## Speedup model (rough estimates)

Baseline: 460-cell grid × 10 s/cell ≈ 4600 s (≈ 75 min) for one
source.

| Combination | Trials | Per-trial cost | Verify cost | Total wall-time | Speedup |
|---|---|---|---|---|---|
| **None (Phase A grid)** | 460 | 10 s (encode + CPU score) | – | 4600 s | 1× |
| **A** (proxy only, dense grid) | 460 | 4 s (encode only; proxy ≈ µs) | 1 s GPU verify | ≈1840 s | ~2.5× |
| **B** (Bayesian, real score) | 50 | 10 s | – | 500 s | ~9× |
| **A + B** (Bayesian + proxy) | 50 | 4 s | 1 s | 201 s | ~23× |
| **A + B + E** (Bayesian + proxy + GPU verify) | 50 | 4 s (encode is the floor) | 0.3 s | 200 s | ~23× |
| **A + B + E + sample-chunk** (5-second sample, full encode-time scales linearly) | 50 | 2 s (5 s clip × ~0.4× realtime) | 0.3 s | ≈100 s | ~46× |
| **A + B + C + E** (NVENC + Bayesian + proxy + GPU verify) | 50 | 0.3 s NVENC + µs proxy | 0.3 s | ≈30 s | ~150× |

The published 50–500× figures from the user's framing assume
lever C (hardware encoder) carries the bulk. The conservative
"no NVENC required" combination still hits 20–50× on a CPU-only
host — which is the baseline the scaffold targets.

**Hard caveat**: these are upper bounds. The proxy's recommendation
is only as good as its calibration. Without a Phase A corpus
trained per-encoder, fr_regressor_v2's predictions on `libx264`
medium / slow at non-training CRFs are an extrapolation. The
production claim has to be validated against Phase A baseline once
the corpus exists; the scaffold ships a smoke-mode that fakes
trials so the pipeline can be exercised without that corpus.

## Decision matrix — which combination is "fast-path v1"?

| Combination | Pros | Cons | Recommendation |
|---|---|---|---|
| **A + B + E (recommended)** | No new external dep beyond Optuna; works on any host; scales gracefully when GPU is absent (lever E is opt-in); the proxy is already-shipped `fr_regressor_v2.onnx` | Speedup capped at ~50×; encode floor remains software | **First scaffold ships A + B + E.** |
| A + B + C + E | Headline 100–500× speedup | NVENC requires an FFmpeg compiled with `--enable-nvenc` and an NVIDIA GPU; QSV/AMF analogues fragment the matrix; the proxy needs a hardware-encoder corpus to be calibrated | Follow-up PR. Requires Phase A.5b corpus regeneration with NVENC. |
| A only (dense grid + proxy) | Zero search-strategy churn; deterministic | Still scans every CRF; barely better than the grid | Rejected; misses the easy Bayesian win. |
| B only (Bayesian + real score) | No proxy calibration risk | 10× speedup but still CPU-bound on encode + score | Backup plan if the proxy's calibration regresses. |
| D (per-shot) bolted onto any | Compounds linearly with shot count | Out of scope for v1; orthogonal | Phase D follow-up. |

The recommended canonical fast-path is **A + B + E** — proxy +
Bayesian + GPU verify — because it ships on any host the rest of
the fork already supports, requires only the existing tiny-AI
ONNX surface plus one new Python dep (`optuna`), and degrades
gracefully on hosts without a GPU verify backend.

## Failure modes the scaffold has to surface

1. **Proxy out-of-distribution**. If a source's canonical-6
   features sit far from the training distribution, the proxy
   will rank candidates incorrectly. The scaffold's verify step
   catches the symptom (predicted VMAF vs measured VMAF
   diverges); the structured response is to fall back to the
   slow grid for that source. The CLI must report the proxy /
   verify gap and exit non-zero past a configurable tolerance.
2. **No Phase A corpus available**. Without a corpus the proxy
   ships with placeholder weights; the fast-path is structurally
   sound but numerically meaningless. The CLI carries an explicit
   `--smoke` flag that skips both encode and proxy and synthesises
   trials, so the pipeline can be exercised in CI before real
   weights exist.
3. **Hardware encoder unavailable**. Lever C is opt-in; defaulting
   to `libx264` keeps the tool runnable on CPU-only hosts. Auto-
   detect via `ffmpeg -encoders` happens in a follow-up.
4. **Optuna missing**. Optuna is an optional runtime dep guarded
   behind an `extras = ["fast"]` block in the package metadata.
   The CLI errors with a clear install-instruction message when
   `vmaf-tune fast` is invoked without the extra installed.

## What the scaffold ships (this PR)

- New `vmaftune.fast` module with a `fast_recommend(...)` entry
  point and a `--smoke` mode that synthesises 50 fake trials and
  runs Optuna over them to validate the pipeline end-to-end.
- `vmaf-tune fast` CLI subcommand wired into the existing argparse
  tree.
- Extended `docs/usage/vmaf-tune.md` with the fast-path section
  and an explicit "what's needed for production" checklist.
- Optional dep on Optuna via `pip install vmaf-tune[fast]`. Core
  install is unchanged.
- ADR-0276 + this digest + AGENTS.md invariant + changelog
  fragment + rebase-notes entry (per ADR-0108).

What is **deferred** to follow-up PRs:

- Real fr_regressor_v2 weights (gated on PR #347 merging plus
  T-VMAF-TUNE-CORPUS-A producing training data).
- ONNX Runtime wiring for the proxy inference call (the scaffold
  uses a stub `predict_vmaf` that returns a deterministic mock
  score so the smoke test stays self-contained).
- NVENC / QSV / AMF auto-detection (lever C).
- Per-shot parallelisation (lever D).
- GPU verify wiring (`vmaf` CLI invocation with `--cuda` /
  `--vulkan` / `--sycl` selected per host capability).
- A Phase B "real recommend" mode that runs the actual encoder +
  proxy loop on a real source.

## References

- `req`: user prompt — "vmaf-tune planning should be GPU/AI fast —
  Netflix-quality decisions in short time, would be huge"
  (paraphrased per [user-quote handling rule](../../CLAUDE.md)).
- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) —
  parent umbrella spec.
- [ADR-0272](../adr/0272-fr-regressor-v2-codec-aware-scaffold.md) —
  fr_regressor_v2 codec-aware scaffold (Phase B prereq).
- [ADR-0249](../adr/0249-fr-regressor-v1.md) — fr_regressor_v1
  baseline (canonical-6 → VMAF).
- [ADR-0235](../adr/0235-codec-aware-fr-regressor.md) — codec-aware
  conditioning shape.
- [ADR-0223](../adr/0223-transnet-v2-shot-detector.md) — TransNet V2
  scaffold (lever D enabler).
- [Research-0044](0044-quality-aware-encode-automation.md) —
  parent option-space digest.
- [Research-0058](0058-fr-regressor-v2-feasibility.md) —
  fr_regressor_v2 feasibility.
- [Optuna documentation](https://optuna.readthedocs.io/) — TPE
  sampler reference.
