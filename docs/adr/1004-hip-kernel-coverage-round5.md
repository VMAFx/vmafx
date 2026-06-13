<!-- markdownlint-disable MD060 -->
# ADR-1004: HIP kernel parity-test coverage round 5

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: lusoris
- **Tags**: `hip`, `tests`, `gpu`, `coverage`, `fork-local`

## Context

ADR-0868 / PR #351 (round 1) added `psnr_hip` + `vif_hip` parity gates.
ADR-0883 / PR #372 (round 2) added `ciede_hip`, `psnr_hvs_hip`,
`motion_hip` (v1), `integer_ssim_hip`, `integer_ms_ssim_hip`. ADR-0945 /
PR #443 (round 3) added `cambi_hip`, `float_adm_hip`, `float_motion_hip`,
`float_psnr_hip`. ADR-0958 / PR #548 (round 4) added `ssimulacra2_hip`,
`float_ssim_hip`.  Together those rounds lifted HIP parity coverage to
15 / 17 extractors (≈88%).

Round 4 originally scoped `speed_chroma_hip` and `speed_temporal_hip` but
deferred them after discovering that `speed_internal_init_dimensions` and
`speed_internal_float_stride` — declared in `core/src/feature/speed_internal.h`
— had no `.c` implementation.  Linking either HIP speed twin into the test
archive produced 4 undefined references and a link failure.  A new
T-HIP-SPEED-INTERNAL-IMPL-MISSING-2026-05-31 row was added to `docs/state.md`
to track the blocker.

ADR-0964 / PR #465 resolved the blocker by introducing
`core/src/feature/speed_internal.c`.  The implementation is a self-contained
port of the static helpers in `speed.c` (the CPU SpEED extractor), placed in a
separate TU to avoid dirtying the Netflix-mirrored `speed.c` on every rebase.
`speed_internal.c` is compiled into libvmaf via `core/src/meson.build` and is
therefore available to every GPU backend that links against the shared library.

With the link defect resolved the two parity tests deferred from round 4 can
now ship.  ADR-0214 requires a synthetic-fixture parity gate for every GPU
extractor before it is considered production-ready.  Round 5 closes the
remaining 2 reachable HIP parity gaps, taking HIP coverage to 17 / 17 (100%)
for all non-deferred extractors.

The `float_moment_hip` extractor remains structurally blocked: its
`provided_features` array (`float_moment_ref1st` / `_dis1st` / `_ref2nd` /
`_dis2nd`) does not share a key with the CPU twin's single `float_moment`
channel, so no shared LHS/RHS surface is available for a parity assertion.
That deferral is tracked separately
(T-HIP-FLOAT-MOMENT-PROVIDED-FEATURES-MISMATCH-2026-05-31 in `docs/state.md`).

## Decision

Add 2 new HIP parity tests under `core/test/`:

- `test_hip_speed_chroma_parity` — `Speed_chroma_feature_speed_chroma_uv_score`,
  places=4 (1e-4). SpEED chroma: tile-parallel GPU mean/covariance/indterm/score;
  CPU-side QR + eigensolver via `speed_internal.c`.  Same arithmetic budget as
  the CUDA `test_cuda_speed_chroma_parity.c` gate.
- `test_hip_speed_temporal_parity` — `Speed_temporal_feature_speed_temporal_score`,
  places=4 (1e-4). SpEED temporal: same hybrid GPU/CPU split; asserts at frame
  index 1 (frame 0 emits a forced-zero score by spec).

Each follows the template established by earlier rounds: 768×432 YUV420P 8 bpc
fixture (matching the CUDA speed tests for cross-backend comparability), CPU
reference vs. HIP score, skip cleanly with `[skip: no HIP device]` when
`vmaf_hip_state_init()` fails OR with `[skip: HIP scaffold ENOSYS]` when the
HIP path returns `-ENOSYS` (scaffold posture under `enable_hipcc=false`).

The stale comment in `core/test/meson.build` describing the round-4 deferral
is updated to note that ADR-0964 resolved the blocker and the gates ship in
this round.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Ship round-5 tests as part of the ADR-0964 / speed_internal.c PR | Single PR closes both the implementation gap and the parity tests | ADR-0964 is a library-implementation PR; bundling test infrastructure complicates review scope | Rejected; ADR-0108 deliverables rule discourages mixing implementation and coverage PRs unless they are trivially related |
| Use places=3 tolerance | Matches the ADR-0958 ssimulacra2 precedent | SpEED's QR / eigensolver runs on CPU for both backends; only per-pixel stats run on GPU; the float arithmetic is identical so places=4 is achievable | Rejected; places=4 is the correct budget per ADR-0214; places=3 would mask regressions |
| One combined test executable for both speed variants | Less meson churn | One skip blocks the other; granularity loss; harder to diagnose regressions per-variant | Rejected; per-kernel split mirrors all prior rounds |

## Consequences

- **Positive**: HIP parity coverage rises from 15/17 → 17/17 (88% → 100%)
  for all non-deferred extractors.  The `speed_chroma_hip` and
  `speed_temporal_hip` gates lock in correctness for the SpEED-QA family
  on AMD GPU, closing the round-4 carryover.
- **Negative**: None significant.  Both tests add ~180 LOC each and link
  against the shared libvmaf archive, so no new static TUs are introduced
  in the test build.
- **Neutral / follow-ups**:
  - `float_moment_hip` parity gate remains deferred pending resolution of
    the CPU/HIP `provided_features` mismatch.
  - The T-HIP-SPEED-INTERNAL-IMPL-MISSING-2026-05-31 row in `docs/state.md`
    is closed by this PR (the defect was fixed by ADR-0964; the test gap
    closes here).
  - Both tests run on the `gpu` + `fast` suites; CI on hosts without an
    AMD GPU exercises only the `[skip: no HIP device]` path.

## References

- Round 1: [ADR-0868](0868-hip-kernel-coverage-round1.md), PR #351
- Round 2: [ADR-0883](0883-hip-kernel-coverage-round2.md), PR #372
- Round 3: [ADR-0945](0945-hip-kernel-coverage-round3.md), PR #443
- Round 4 (origin of speed-family deferral):
  [ADR-0958](0958-hip-kernel-coverage-round4.md), PR #548
- `speed_internal.c` implementation (unblocked link): PR #465
  [ADR-0964](0964-implement-speed-internal-and-wire-gpu-speed-extractors.md)
- Backend tolerance policy: [ADR-0214](0214-gpu-numerical-tolerance.md)
- Source: per user direction (HIP kernel coverage round 5 dispatch)
