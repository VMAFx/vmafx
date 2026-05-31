<!-- markdownlint-disable MD060 -->
# ADR-0304: `vmaf-tune fast` — production wiring (Optuna TPE + v2 proxy + GPU verify)

- **Status**: Accepted
- **Date**: 2026-05-05
- **Deciders**: Lusoris
- **Tags**: tooling, ai, ffmpeg, codec, automation, fork-local

## Context

[ADR-0276](0276-vmaf-tune-fast-path.md) shipped the `vmaf-tune fast`
subcommand as a **scaffold** under
`tools/vmaf-tune/src/vmaftune/fast.py`: smoke mode runs Optuna over a
synthetic CRF→VMAF curve, but `smoke=False` raises
`NotImplementedError` because no production proxy or verify pass was
wired. The scaffold deliberately deferred the production loop to a
follow-up gated on two prerequisites:

1. **A real Phase A corpus.** PR #392 produced
   `hw_encoder_corpus.py` (33,840 per-frame canonical-6 rows across
   9 Netflix sources × NVENC + QSV); see
   [ADR-0291](0291-fr-regressor-v2-prod-ship.md).
2. **A production fr_regressor_v2.** ADR-0291 flipped the v2 row in
   `model/tiny/registry.json` from `smoke: true` to `smoke: false`,
   shipping the trained ONNX (sha256
   `67934b0b…`) at LOSO PLCC ≥ 0.95. The model predicts VMAF from
   six canonical libvmaf features (adm2, vif_scale0..3, motion2) plus
   a 14-D codec block (12-way encoder one-hot + preset_norm +
   crf_norm).

Both prerequisites are now satisfied. This ADR records the
decision to wire the production loop on top of the scaffold without
relaxing the scaffold's invariants — `fast` stays opt-in, the slow
grid stays canonical, and the smoke-mode entry point keeps working
on hosts without Optuna or onnxruntime installed.

[Research-0076](../research/0076-vmaf-tune-fast-path-prod-wiring.md)
is the companion digest. It walks the n_trials-vs-convergence
trade-off for TPE on a CRF axis (typically 30–50 trials before
diminishing returns), the proxy-vs-truth correlation budget (mean
absolute VMAF gap ≤ 0.5 expected at v2's PLCC of 0.9794), and the
single-pass GPU-verify cost (one ffmpeg encode + one libvmaf score
at the chosen CRF, end-to-end seconds on CUDA / Vulkan / SYCL).

## Decision

We will wire the production fast-path loop as follows:

1. **Search strategy: Optuna TPE.** The scaffold already imports
   Optuna; we keep `optuna.samplers.TPESampler(seed=0)` as the
   default sampler. Bayesian search beats grid + random on an
   integer-CRF axis at this trial budget; CMA-ES is overkill for a
   single integer dimension.
2. **Proxy scorer: `fr_regressor_v2`.** The proxy is the production
   ONNX shipped in ADR-0291 (no smoke models). A new
   `vmaftune.proxy.run_proxy(features, codec_block) -> float` helper
   loads the registry-pinned ONNX session lazily and runs inference;
   the helper is the single seam every consumer goes through, so
   future ensemble / probabilistic-head migrations (ADR-0279
   follow-up) land in one place.
3. **Single GPU verify pass at recommend-end.** After TPE converges,
   the harness runs **one** real encode + libvmaf score at the
   recommended CRF using the GPU score backend selected via
   `vmaftune.score_backend.select_backend(prefer)` (ADR-0299).
   The proxy score and the verify score are both reported; the
   *verify* score is authoritative, the proxy score is logged as a
   diagnostic. Proxy alone never wins — the contract is one verify
   pass, always, regardless of how confident the proxy looks.
4. **Smoke mode keeps working.** `fast_recommend(..., smoke=True)`
   still routes through the synthetic `_smoke_predictor` and skips
   the verify pass. CI on hosts without onnxruntime / Optuna / a
   GPU continues to exercise the search-loop wiring end-to-end.

The recommended-CRF result gains two new fields beyond the scaffold's
shape: `verify_vmaf` (the real libvmaf score from the GPU pass) and
`proxy_verify_gap` (`abs(predicted_vmaf - verify_vmaf)`). When the
gap exceeds a configurable tolerance the CLI exits non-zero so the
operator knows the proxy was OOD on this source — this is the
explicit fallback signal to the slow Phase A grid (ADR-0276
"Negative consequences" mitigation).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Optuna TPE (chosen)** | Bayesian convergence in 30–50 trials on an integer CRF axis; scaffold already imports it; deterministic with seed; proven on similar tuning loops | Optional Optuna dep (already in `[fast]` extra) | Picked: best convergence-per-trial on this dimension; matches ADR-0276 scaffold |
| CMA-ES | Robust on continuous high-dim spaces | Overkill for a single integer dimension; CRF discretisation defeats its strength; no convergence advantage at 30–50 trials | Rejected: wrong tool for the problem geometry |
| Random + early-stop | Zero new search complexity; trivially parallelisable | Needs ~3× more trials than TPE for the same convergence; no Bayesian "sharpening" near the target VMAF | Rejected: wastes the proxy budget on uninformative samples |

## Consequences

- **Positive**:
  - The recommendation use case becomes seconds-to-minutes (TPE
    typically 30–50 trials × proxy inference µs + 1 GPU verify pass)
    instead of hours (Phase A grid).
  - The proxy/verify gap surfaces in the result, giving the operator
    an explicit OOD signal without silent failure.
  - Production fr_regressor_v2 finally has a user-facing application,
    validating ADR-0291's ship gate end-to-end.
  - Smoke mode is preserved as the CI-friendly path.
- **Negative**:
  - `vmaf-tune[fast]` now needs onnxruntime in addition to Optuna for
    the production path. CI hosts without onnxruntime fall back to
    smoke mode; tests skip the production-path cases via
    `pytest.importorskip`.
  - The proxy is bounded by v2's calibration. Out-of-distribution
    sources (HDR, highly synthetic content, novel codecs not in
    ENCODER_VOCAB v2) can produce a large proxy/verify gap. Mitigation:
    the gap is logged and gates the exit code.
- **Neutral / follow-ups**:
  - NVENC / QSV / AMF auto-detection (lever C in ADR-0276) remains a
    follow-up. Today the verify pass uses the same encoder the proxy
    was trained on.
  - Probabilistic-head proxy (ADR-0279, ensemble) is a follow-up. The
    `run_proxy` seam abstracts the inference call so the swap is
    one-file.
  - A small recommendation-quality benchmark — for ≥3 sources, run
    both Phase A grid and `fast`, report the verify VMAF gap at the
    recommended CRF — must land before Status flips to Accepted.

## References

- `req`: user prompt — paraphrased: "wire the vmaf-tune fast-path
  production code paths now that fr_regressor_v2 is production —
  Optuna TPE search using the v2 proxy, then a single GPU-verify
  pass" (paraphrased per the
  [user-quote handling rule](../../CLAUDE.md)).
- [ADR-0276](0276-vmaf-tune-fast-path.md) — fast-path scaffold (this
  ADR's parent).
- [ADR-0237](0237-quality-aware-encode-automation.md) — vmaf-tune
  umbrella spec.
- [ADR-0291](0291-fr-regressor-v2-prod-ship.md) — fr_regressor_v2
  production ship (this ADR's proxy prerequisite).
- [ADR-0299](0299-vmaf-tune-gpu-score.md) — GPU score backend
  selection (this ADR's verify-pass enabler).
- [Research-0076](../research/0076-vmaf-tune-fast-path-prod-wiring.md)
  — companion digest.
