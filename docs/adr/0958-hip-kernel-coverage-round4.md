<!-- markdownlint-disable MD060 -->
# ADR-0958: HIP kernel parity-test coverage round 4

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `hip`, `tests`, `gpu`, `coverage`, `fork-local`

## Context

ADR-0868 / PR #351 (round 1) added `psnr_hip` + `vif_hip` parity gates.
ADR-0883 / PR #372 (round 2) added `ciede_hip`, `psnr_hvs_hip`,
`motion_hip` (v1), `integer_ssim_hip`, `integer_ms_ssim_hip`. ADR-0945 /
PR #443 (round 3) added `cambi_hip`, `float_adm_hip`, `float_motion_hip`,
`float_psnr_hip`. Together those rounds lifted HIP parity coverage to
13 / 17 extractors (≈76%).

Round 4 was originally scoped to close the remaining 4 reachable HIP
kernels: `ssimulacra2_hip`, `float_ssim_hip`, `speed_chroma_hip`,
`speed_temporal_hip`. Verification during the container build (vmaf-dev-mcp
container, `enable_hip=true enable_hipcc=false`) surfaced two distinct
findings:

1. **`ssimulacra2_hip` and `float_ssim_hip` close cleanly**. Both ship
   stable CPU twins (`vmaf_fex_ssimulacra2`, `vmaf_fex_float_ssim`) with
   `provided_features` arrays that match the HIP extractor's emitted
   feature keys (`ssimulacra2`, `float_ssim`). The parity tests build,
   link, and exercise the `[skip: no HIP device]` path correctly on
   CPU-only hosts.

2. **`speed_chroma_hip` and `speed_temporal_hip` are blocked on a
   pre-existing latent link defect**. Hypothesis: the round-3 audit's
   claim that the speed-family lacked a CPU scalar reference was
   incorrect (CPU twins do exist at `core/src/feature/speed.c:1335` and
   `:1559`), so wiring up the parity tests should be routine.
   Verification: building with `speed_chroma_hip.c` /
   `speed_temporal_hip.c` added to the HIP archive surfaced 4 undefined
   references at link time:

   ```text
   undefined reference to `speed_internal_init_dimensions'
   undefined reference to `speed_internal_float_stride'
   ```

   These helpers are declared in
   `core/src/feature/speed_internal.h` (lines 85, 93) but no `.c`
   implementation exists anywhere in `core/src/feature/`. The
   `speed_*_cuda.c` and `speed_*_sycl.cpp` twins reference the same
   helpers, so the bug is latent on all three GPU backends — none of
   the GPU speed-family TUs are currently wired into their respective
   meson archives. Fixing it requires extracting `init_dimensions` and
   `float_stride` (and likely siblings) from `speed.c`'s
   non-exported helpers into a new `speed_internal.c`. That work is
   non-test-shaped and out of scope for a coverage PR.

3. **`float_moment_hip` remains structurally blocked** (the round-3
   deferral reason still applies). Its `provided_features` array
   (`float_moment_ref1st` / `_dis1st` / `_ref2nd` / `_dis2nd`) does
   not share a key with the CPU twin's single `float_moment` channel,
   so a parity gate has no shared LHS/RHS surface to assert against.

Per ADR-0214 every GPU extractor needs a synthetic-fixture parity
assertion before it is considered production-ready. Round 4 closes
the 2 unblocked kernels, taking HIP parity coverage from
13 / 17 → 15 / 17 (≈88%). The remaining 3 deferrals are tracked
under new state.md rows:

- `speed_chroma_hip` / `speed_temporal_hip`:
  T-HIP-SPEED-INTERNAL-IMPL-MISSING-2026-05-31 (carryover from this
  PR's investigation).
- `float_moment_hip`:
  T-HIP-FLOAT-MOMENT-PROVIDED-FEATURES-MISMATCH-2026-05-31 (already
  on the deferral list from round 3).

## Decision

Add 2 new HIP parity tests under `core/test/`:

- `test_hip_ssimulacra2_parity` — `ssimulacra2`, places=3 (1e-3).
  6-scale Gaussian pyramid with SSIM-like multiplicative pooling; the
  rounding budget mirrors MS-SSIM.
- `test_hip_float_ssim_parity` — `float_ssim`, places=3 (1e-3).
  Float-pipeline SSIM accumulates per-window rounding across the
  luminance / contrast / structure terms; same convention as ADR-0883's
  integer SSIM gate.

Each follows the template established by `test_hip_vif_parity.c`
(PR #351), `test_hip_motion_parity.c` (PR #372), and
`test_hip_float_psnr_parity.c` (PR #443): synthetic YUV420P fixture,
CPU reference vs. HIP score, skip cleanly with `[skip: no HIP device]`
when `vmaf_hip_state_init()` fails OR when the HIP path returns
`-ENOSYS` (scaffold posture under `enable_hipcc=false`).

Fixture geometry: 256x144 YUV420P 8 bpc for both extractors (matches
the round-1/2/3 template; well above the 8x8 lower bound enforced by
both `init_fex_hip()` paths).

Tolerances per ADR-0214: places=3 for both extractors — the multi-scale
pooling (ssimulacra2) and windowed pooling (float_ssim) accumulate
per-window rounding comparable to MS-SSIM (round-2 places=3 precedent).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Ship 4 tests as originally planned (include `speed_chroma_hip` + `speed_temporal_hip`) | Closes the round in one batch | Tests can't link — `speed_internal_init_dimensions` / `speed_internal_float_stride` undefined; CI would red on first build | Rejected; verified via container build (vmaf-dev-mcp `enable_hip=true enable_hipcc=false`). The link defect must be fixed in a separate PR that adds `core/src/feature/speed_internal.c` |
| Stub out the missing helpers in this PR | Single-PR closeout | Requires extracting algorithm-defining logic from `speed.c`; landing untested helpers behind a coverage PR violates touched-file lint rule (no functional change without ADR + tests) | Rejected; the helpers are non-trivial (resolution math + alignment) and need their own design review |
| Include `float_moment_hip` in round 4 | Closes the very last gap | CPU and HIP `provided_features` arrays do not match — parity gate has no shared LHS/RHS channel | Rejected; needs an API-shape decision (split CPU or collapse HIP) before any parity gate is meaningful |
| Use places=4 for SSIMULACRA 2 | Tighter parity guarantee | The 6-scale pyramid accumulates more rounding than even MS-SSIM; places=4 likely flaky on AMD GPU | Rejected; places=3 matches MS-SSIM gate precedent and the SS2 algorithm's own per-scale pooling shape |
| One mega-test for both kernels | Single executable, less meson churn | One skip blocks the other; granularity loss | Rejected; per-kernel split mirrors rounds 1/2/3 |

## Consequences

- **Positive**: HIP parity coverage rises from 13/17 → 15/17
  (76% → 88%). The `ssimulacra2_hip` gate locks in correctness for
  the only HIP extractor that ships a fork-original perceptual metric
  (vs. upstream-derived metrics). The `float_ssim_hip` gate closes
  the SSIM family on HIP (integer SSIM was covered in round 2).
- **Negative**: Adds ~400 LOC of test code (down from the originally
  planned ~800). Round 4 ships smaller than rounds 1-3 because the
  speed-family discovery surfaced a deeper structural bug that
  belongs in its own PR.
- **Neutral / follow-ups**:
  - `speed_chroma_hip` / `speed_temporal_hip` parity gates deferred
    pending `core/src/feature/speed_internal.c` introduction. The
    same defect blocks CUDA + SYCL speed-family parity gates; a
    single fix unblocks all three backends.
  - `float_moment_hip` parity gate deferred pending the CPU/HIP
    `provided_features` mismatch being resolved.
  - The 2 new gates run on the `gpu` test suite + the `fast` suite;
    CI on hosts without an AMD GPU exercises only the
    `[skip: no HIP device]` path (no false positives, no false
    negatives).

## References

- Round 1: [ADR-0868](0868-hip-kernel-coverage-round1.md), PR #351
- Round 2: [ADR-0883](0883-hip-kernel-coverage-round2.md), PR #372
- Round 3: [ADR-0945](0945-hip-kernel-coverage-round3.md), PR #443
- Backend tolerance policy: [ADR-0214](0214-gpu-numerical-tolerance.md)
- HIP backend audit motivating the rounds: Research-0755,
  `docs/research/0755-hip-backend-audit-20260529.md`
- Speed-family Python compat (CPU twin port):
  T-SPEED-PYTHON-COMPAT-2026-05-28 in `docs/state.md`
- Research digest for this round:
  `docs/research/0958-hip-kernel-coverage-round4-2026-05-31.md`
- Source: per user direction (HIP kernel coverage round 4 dispatch)
