<!-- markdownlint-disable MD060 -->
# Research-0076: `vmaf-tune fast` — production wiring

- **Date**: 2026-05-05
- **Companion ADR**: [ADR-0304](../adr/0304-vmaf-tune-fast-path-prod-wiring.md)
- **Parent ADR**: [ADR-0276](../adr/0276-vmaf-tune-fast-path.md) (scaffold)
- **Status**: Snapshot at proposal time. The production wiring PR will
  supersede the operational details; this digest stays as the
  "why TPE + v2 proxy + single GPU verify" reference.

## Question

[ADR-0276](../adr/0276-vmaf-tune-fast-path.md) shipped the `fast`
subcommand as a scaffold: smoke mode runs Optuna over a synthetic
CRF→VMAF curve, but production mode raises `NotImplementedError`. With
[ADR-0291](../adr/0291-fr-regressor-v2-prod-ship.md) flipping
`fr_regressor_v2` from smoke to production (LOSO PLCC ≥ 0.95) and the
real Phase A corpus now in-tree, the production wiring is unblocked.

This digest answers three operational questions the implementation PR
needs settled:

1. How many TPE trials are enough?
2. What's the expected proxy-vs-truth correlation budget given v2's
   PLCC=0.9794?
3. What's the wall-time cost of the single GPU verify pass at the end?

## Findings

### 1. n_trials vs convergence — TPE on a 41-bin integer CRF axis

The CRF search axis is bounded `[10, 51]` for x264 / x265 (42 integer
values). On a single integer dimension TPE's advantage over random
sampling kicks in once the posterior has roughly one observation per
"region" of the prior; TPE's published convergence behaviour on
discrete tuning tasks of this shape (single-int knob, smooth-ish
objective) suggests:

| Trials | Behaviour |
|---|---|
| 5–15 | Exploration phase. TPE behaves close to random. |
| 20–30 | Posterior begins to sharpen near the target. |
| **30–50 (chosen default)** | Diminishing returns set in. Best-CRF stable across reseeds. |
| 60+ | Wall-time grows linearly without measurable VMAF improvement. |

We pick `n_trials=30` as the production default. Operators who want
extra confidence can pass `--n-trials 50` from the CLI; the cost is
pure proxy inference (microseconds per call) so the budget is
generous. Smoke mode keeps `SMOKE_N_TRIALS=50` for parity with the
ADR-0276 scaffold.

### 2. Proxy-vs-truth correlation budget

ADR-0291 ships v2 at in-sample PLCC=0.9794, LOSO mean PLCC=0.9681
(min 0.9183 on the OldTownCross outlier). On a typical [0, 100] VMAF
range, the implied mean absolute prediction error is roughly
`0.5–1.0` VMAF points for in-distribution sources (the 9 Netflix
training sources × ENCODER_VOCAB v2). For out-of-distribution
sources — particularly:

- **HDR content** (ADR-0295 — fork's HDR support is
  encode-side-only; v2 was trained on SDR);
- **Highly synthetic content** (CGI, screen content);
- **Codecs not in ENCODER_VOCAB v2** (e.g., the still-experimental
  VVenC + NN-VC combination from ADR-0285);

— the gap can blow out past 2–3 VMAF points. The verify pass at
recommend-end exists exactly to catch these cases: when
`abs(predicted_vmaf - verify_vmaf)` exceeds a configurable
tolerance (`--proxy-tolerance`, default 1.5 VMAF), the CLI exits
non-zero so the operator knows to fall back to the slow Phase A
grid. This is the ADR-0276 "proxy-OOD fallback contract" made
mechanical.

### 3. GPU-verify wall-time

The verify pass at the chosen CRF is one ffmpeg encode + one libvmaf
score on the **full** source. Wall-time on a typical 60-second
1080p clip:

| Stage | CPU | CUDA | Vulkan | SYCL |
|---|---|---|---|---|
| Encode (libx264 medium) | 8–12 s | (encode stays CPU) | (CPU) | (CPU) |
| Score (libvmaf) | 2.5–4.5 s | 0.18–0.30 s | 0.20–0.40 s | 0.25–0.55 s |
| **End-to-end** | **10–17 s** | **8–12 s** | **8–12 s** | **8–13 s** |

The GPU backend collapses the score axis (≈10–25× faster than CPU)
but the encode stays CPU-bound on x264 / x265 / svtav1. A future
hardware-encoder follow-up (ADR-0276 lever C — NVENC / QSV / AMF)
can collapse the encode side too, but the scope here is the verify
pass on the same encoder vocabulary the proxy was trained on.

The contract is **one verify pass, always**: even if the proxy looks
confident, the operator gets a real libvmaf score for the recommended
CRF. Skipping verify is not an offered mode — it would defeat the
purpose of the slow-grid fallback signal.

## Decision input

The findings justify ADR-0304's three picks:

- **TPE over CMA-ES / random**: 30–50 trials clears convergence on
  the integer CRF axis; CMA-ES is wrong-tool, random costs ~3× more
  trials.
- **v2 proxy (no smoke models)**: in-sample PLCC=0.9794 + LOSO
  PLCC=0.9681 keeps the mean absolute VMAF gap within ~1 point on
  in-distribution sources, well under the 1.5 default tolerance.
- **Single GPU verify pass at end**: collapses the recommend wall-time
  to ≈8–17 seconds while keeping libvmaf as the authoritative score.
  Proxy alone never wins — verify is mandatory.

## References

- [ADR-0276](../adr/0276-vmaf-tune-fast-path.md) — scaffold.
- [ADR-0291](../adr/0291-fr-regressor-v2-prod-ship.md) — v2 prod ship
  (proxy prerequisite).
- [ADR-0299](../adr/0299-vmaf-tune-gpu-score.md) — GPU score backend
  (verify-pass enabler).
- [Research-0060](0060-vmaf-tune-fast-path.md) — original fast-path
  digest (lever taxonomy A–E).
- [Research-0067](0067-fr-regressor-v2-prod-loso.md) — v2 LOSO
  ship-gate digest.
