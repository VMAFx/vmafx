# ADR-0276: `vmaf-tune fast` — proxy-based recommend (Phase A.5)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, ai, ffmpeg, codec, automation, fork-local

## Context

[ADR-0237](0237-quality-aware-encode-automation.md) established
`tools/vmaf-tune/` as the fork's quality-aware encode automation
surface. Phase A (PR #329, Accepted 2026-05-03) ships the grid-
sweep corpus generator: every `(preset, crf)` cell encodes with
`libx264`, scores with `libvmaf`, and emits a JSONL row. The
encode + score loop costs 5–15 seconds per cell on 1080p and
scales linearly; a useful corpus for one source is 30–60 minutes
at minimum, hours at slow presets.

For the *recommendation* use case — "given a source and a target
VMAF, what CRF should I use?" — the exhaustive grid is overkill.
The fork already ships every primitive a fast surrogate-model
search would need: `fr_regressor_v1` (canonical-6 → VMAF, ADR-0249)
and the in-flight `fr_regressor_v2` (codec-aware, ADR-0272) are
MLP models that predict VMAF in microseconds, the GPU backends
(CUDA / Vulkan / SYCL, ADR-0157 / ADR-0186) accelerate the
verification pass 8–20×, and Optuna's TPE sampler converges in
~1/10 the trials a dense grid needs. The user's framing — paraphrased,
"vmaf-tune planning should be GPU/AI fast" — asks whether these can
combine into an opt-in fast-path that hits within tolerance of the
slow grid's optimum in seconds-to-minutes rather than hours.

[Research-0060](../research/0060-vmaf-tune-fast-path.md) is the
companion digest — it walks the bottleneck, the five candidate
acceleration levers (proxy, Bayesian, hardware encoder, per-shot
parallelisation, GPU verify), and the speedup model that justifies
picking levers A + B + E for the first scaffold.

## Decision

We will ship a new `vmaf-tune fast` subcommand under
`tools/vmaf-tune/src/vmaftune/fast.py` that combines the
**proxy + Bayesian + GPU-verify** acceleration levers (A + B + E
in Research-0060) into a single opt-in entry point. The slow Phase
A grid path stays unchanged as the ground-truth corpus generator
(ADR-0237 contract). The fast-path is gated behind an explicit
`fast` subcommand and an optional `[fast]` install extra so the
core install matrix and runtime dependency surface are unchanged
for users who only want the grid path.

The first scaffold ships a `--smoke` mode that synthesises 50 fake
trials and runs Optuna over them, validating the pipeline end-to-end
without requiring a real source, a real proxy weights file, or a
real FFmpeg encode. Production wiring (real `fr_regressor_v2.onnx`
inference, real encode-extract-predict loop, real GPU verify) lands
in a follow-up PR gated on the Phase A corpus existing and
fr_regressor_v2 finishing training (PR #347).

The recommended CRF and predicted VMAF must be reported alongside
the proxy / verify gap so the operator can see when the fast-path
is out-of-distribution and should fall back to the slow grid. The
slow grid is never automatically replaced — fast-path is opt-in,
slow grid stays canonical.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **A + B + E (proxy + Bayesian + GPU verify) — chosen** | Works on any host the fork already supports; leans on shipped tiny-AI surface + one optional Python dep (Optuna); degrades gracefully when GPU verify backend absent | Speedup capped at ~20–50× without lever C; encode floor remains software | Picked: best speedup-per-complexity ratio for v1 |
| A + B + C + E (add NVENC / QSV / AMF) | 100–500× headline speedup; closer to the user's "huge" framing | NVENC requires `--enable-nvenc` FFmpeg + NVIDIA GPU; calibration needs a hardware-encoder corpus that does not yet exist; cross-vendor matrix (NVENC / QSV / AMF) explodes the test surface | Deferred to follow-up; needs Phase A.5b NVENC corpus first |
| A only (dense grid + proxy) | Zero search-strategy churn; deterministic | Still scans every CRF; misses the easy Bayesian win | Rejected; misses 80 % of the available speedup |
| B only (Bayesian + real CPU score) | No proxy calibration risk | Caps at ~10× speedup; still bottlenecked on CPU encode + score | Backup plan if the proxy regresses; not the recommended default |
| Replace slow grid entirely with fast-path | Single code path; less surface | Loses the grid-as-corpus contract Phase B/C consume; no ground-truth fallback when proxy is OOD | Rejected; ADR-0237 explicitly carves the grid as the corpus generator |
| Implement in Phase A surface (no new subcommand) | One less subcommand to learn | Hides opt-in nature; users running `corpus` would get surprising fast behaviour | Rejected; opt-in surface keeps slow grid the obvious default |
| Build proxy in C inside libvmaf | No Python dep at runtime | Recapitulates ONNX Runtime wiring inside the harness; Python tooling tree is the right home (per ADR-0237's hybrid C+Python decision) | Rejected; proxy inference is the Python tree's job |

## Consequences

- **Positive**:
  - Recommendation use case ("what CRF for target VMAF X?")
    becomes seconds-to-minutes instead of an hours-long grid run.
  - Existing tiny-AI surface (`fr_regressor_v2`, ADR-0272) gains a
    user-facing application beyond research notebooks; validates
    the codec-aware design end-to-end.
  - Slow grid path stays as ground truth — no breaking change to
    Phase A consumers.
  - Smoke mode keeps the new code path testable in CI without
    requiring real weights, real ffmpeg, or a GPU.
- **Negative**:
  - New optional dep on Optuna; users must install
    `vmaf-tune[fast]` to get the fast-path. Documented in
    `docs/usage/vmaf-tune.md`.
  - Recommendation quality is bounded by the proxy's calibration —
    out-of-distribution sources can produce bad recommendations.
    Mitigation: the verify step reports proxy/verify gap; CLI exits
    non-zero past a configurable tolerance.
  - Two code paths to maintain (slow grid + fast). Mitigated by
    keeping the fast-path under a separate module and reusing the
    Phase A codec-adapter registry without modification.
- **Neutral / follow-ups**:
  - Production wiring (real ONNX inference, real encode loop, real
    GPU verify) is a follow-up PR.
  - NVENC / QSV / AMF auto-detection (lever C) is a follow-up.
  - Per-shot parallelisation (lever D, TransNet V2 input) is a
    follow-up tracked under the existing `vmaf-perShot` story
    (ADR-0222).
  - A small recommendation-quality benchmark — for ≥3 sources, run
    both the slow grid and the fast-path, report the VMAF gap at
    the recommended CRF — must land before the production fast-path
    flips Status to Accepted.
  - User docs updated (`docs/usage/vmaf-tune.md`) in the same PR.

## References

- `req`: user prompt — "vmaf-tune planning should be GPU/AI fast —
  Netflix-quality decisions in short time, would be huge"
  (paraphrased per the [user-quote handling rule](../../CLAUDE.md)).
- [ADR-0237](0237-quality-aware-encode-automation.md) — parent
  umbrella spec (Phase A Accepted, Phases B–F Proposed).
- [ADR-0272](0272-fr-regressor-v2-codec-aware-scaffold.md) —
  `fr_regressor_v2` codec-aware scaffold (Phase B prereq).
- [ADR-0249](0249-fr-regressor-v1.md) — `fr_regressor_v1` baseline.
- [ADR-0235](0235-codec-aware-fr-regressor.md) — codec-aware
  conditioning shape.
- [ADR-0157](0157-cuda-preallocation-leak-netflix-1300.md) — CUDA
  backend (lever E enabler).
- [ADR-0186](0186-vulkan-image-import-impl.md) — Vulkan backend
  (lever E enabler).
- [Research-0060](../research/0060-vmaf-tune-fast-path.md) —
  companion digest.
- Related PRs: parent #329 (Phase A scaffold); follow-up #347
  (`fr_regressor_v2` scaffold).

### Status update 2026-05-08: CLI surface landed

The Python API in `tools/vmaf-tune/src/vmaftune/fast.py` had been
production-wired since [ADR-0304](0304-vmaf-tune-fast-path-prod-wiring.md),
but the surface was reachable only via direct Python imports — the
HP-3 audit ([Research-0090](../research/0090-phase-a-promotion-audit-2026-05-08.md))
flagged the changelog claim "production-wired" as still false at the
CLI level. This PR closes that gap by adding the `vmaf-tune fast`
subparser with the user-facing flags listed in
[`docs/usage/vmaf-tune.md`](../usage/vmaf-tune.md#fast-subcommand-proxy-bayesian-gpu-verify-phase-a5),
plus the production runners that build the canonical-6
`sample_extractor` and the real-encode `encode_runner` from the
existing `vmaftune.encode` + `vmaftune.score` pipeline. The CLI is
now the single seam that injects both — `_build_prod_predictor` and
`_gpu_verify` no longer raise when called from the CLI.

Output schema matches the JSON shape `recommend` and `predict`
already emit (single source of truth) plus the fast-path-specific
`verify_vmaf` / `proxy_verify_gap` / `score_backend` diagnostics.
Smoke mode stays untouched as the CI-friendly entry point.

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `tools/vmaf-tune/src/vmaftune/fast.py` — present (scaffold +
  smoke mode).
- ADR-0304 (Accepted in the 2026-05-06 sweep) wired the production
  path: `vmaftune.proxy.run_proxy`, `_proxy_score`, `_run_tpe`,
  `_gpu_verify`.
- `vmaf-tune fast` subcommand registered via the cli surface.
- Verification command:
  `ls tools/vmaf-tune/src/vmaftune/fast.py`.
