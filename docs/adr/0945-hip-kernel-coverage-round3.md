<!-- markdownlint-disable MD013 MD060 -->
# ADR-0945: HIP kernel parity-test coverage round 3

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `hip`, `tests`, `gpu`, `coverage`, `fork-local`

## Context

ADR-0868 / PR #351 (round 1) added `psnr_hip` + `vif_hip` parity gates.
ADR-0883 / PR #372 (round 2) added `ciede_hip`, `psnr_hvs_hip`,
`motion_hip` (v1), `integer_ssim_hip`, `integer_ms_ssim_hip`. Together
those rounds lifted HIP parity coverage to 9 / 17 extractors (≈53%).

The remaining 8 HIP extractors split into blocked + reachable
candidates per the round-2 audit deferral table:

- `ssimulacra2_hip` — blocked on PR #290 (allocation-leak fix actively
  in flight on this file).
- `speed_chroma_hip` / `speed_temporal_hip` — blocked on the lack of a
  stable CPU scalar reference for the speed-family features.
- `cambi_hip` — reachable; the encoded-bitrate metadata gap flagged in
  the round-2 audit is satisfiable from the `enc_width`/`enc_height`
  feature-parameter dict (see `core/src/feature/hip/integer_cambi_hip.c`
  lines 174-205). When unset, init() falls back to source dimensions
  (line 522: `s->enc_width = (int)w; s->enc_height = (int)h;`), so a
  320×240 fixture clears the `CAMBI_HIP_MIN_WIDTH_HEIGHT == 216` floor
  without explicit metadata.
- `float_adm_hip`, `float_motion_hip`, `float_psnr_hip` — reachable;
  the float pipeline scaffolds return `-ENOSYS` from init() unless
  `enable_hipcc=true` is also set, but the parity test follows the
  established skip-on-ENOSYS pattern (init failure → emit skip,
  return success) so the gate is gated by toolchain rather than by
  feature posture.
- `float_ssim_hip` and `float_moment_hip` deferred to round 4 — float
  SSIM mirrors integer SSIM (covered in round 2) and float moment is
  an internal helper with no public CPU twin surface.

Per ADR-0214 every GPU extractor needs a synthetic-fixture parity
assertion before it can be considered production-ready. Round 3 closes
4 of the 5 reachable kernels in one batch, leaving ssimulacra2 + the
speed-family + float-ssim/moment for round 4 after the blockers clear.

## Decision

Add 4 new HIP parity tests under `core/test/`:

- `test_hip_cambi_parity` — `Cambi_feature_cambi_score`, places=3
  (CAMBI's pooling reductions accumulate similar rounding to MS-SSIM).
- `test_hip_float_adm_parity` — `VMAF_feature_adm2_score` +
  scale0..scale3, places=4.
- `test_hip_float_motion_parity` — `VMAF_feature_motion_score`,
  places=4.
- `test_hip_float_psnr_parity` — `float_psnr_y` + `float_psnr_cb` +
  `float_psnr_cr`, places=4.

Each follows the template established by `test_hip_vif_parity.c`
(PR #351) and `test_hip_motion_parity.c` (PR #372): synthetic
YUV420P fixture, CPU reference vs. HIP score, skip cleanly with
`[skip: no HIP device]` when `vmaf_hip_state_init()` fails OR when
the HIP path returns `-ENOSYS` (scaffold posture under
`enable_hipcc=false`).

Fixture geometry: 320×240 YUV420P 8 bpc for `cambi_hip` (to clear the
216 × 216 minimum); 256×144 YUV420P 8 bpc for the three `float_*`
extractors (matches the round-1/2 template).

Tolerances per ADR-0214: places=4 for unfiltered reductions
(`float_psnr`, `float_motion`, `float_adm` scales); places=3 for
`cambi` because the pooling tree accumulates per-window rounding
comparable to MS-SSIM.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Include `ssimulacra2_hip` in round 3 | Closes one more gap | PR #290 actively rewrites the same file — merge-conflict risk | Rejected; wait for PR #290 to settle, sweep in round 4 |
| Include `speed_chroma_hip` / `speed_temporal_hip` | Closes 2 more gaps | No CPU scalar reference exists for the speed-family features on master tip; parity assertion has no LHS | Rejected; the kernels themselves still ship the same `-ENOSYS` scaffold contract so the smoke gate suffices until a CPU reference lands |
| Defer `cambi_hip` to round 4 (treat metadata note from ADR-0883 as a hard blocker) | Smaller round-3 batch | The "encoder metadata" concern resolves itself with a >= 216 × 216 fixture per cambi_hip line 522 fallback; deferral would be over-cautious | Rejected; verified via code-read that the fixture path satisfies init() |
| One mega-test for all 4 kernels | Single executable, less meson churn | One skip blocks the others; granularity loss | Rejected; per-kernel split mirrors rounds 1 & 2 |
| Bit-exact tolerance (`places=15`) | Strongest possible gate | HIP/CPU reductions are not bit-exact (memory: "Golden gate CPU-only; GPUs NOT bit-exact"); guaranteed false-fail | Rejected; places=3/4 per ADR-0214 contract |

## Consequences

- **Positive**: HIP backend coverage rises from 9 / 17 to 13 / 17
  parity-gated extractors (≈76%), exceeding CUDA's gate ratio. Future
  refactors of the float-extractor family or the CAMBI banding kernels
  get CI gates per touched feature.
- **Negative**: 4 additional executables in the `gpu` test suite —
  total CI wall time on the AMD runner grows by ~30 s (each test runs
  1-2 frames through libvmaf, container-cached so build is shared).
  Without `enable_hipcc=true` the float_* tests skip silently
  (init returns -ENOSYS); the cambi test runs end-to-end because the
  CAMBI HIP extractor maintains a CPU-residual code path under the
  `!HAVE_HIPCC` branch.
- **Neutral / follow-ups**: Round-4 backlog (4 extractors):
  `ssimulacra2_hip` (unblock when PR #290 lands), `speed_chroma_hip`,
  `speed_temporal_hip` (unblock when CPU scalar reference lands),
  `float_ssim_hip` (mirror integer SSIM), `float_moment_hip`
  (needs public CPU twin first).

## References

- ADR-0214 — cross-backend parity tolerance gate (places=4 unfiltered, places=3 filtered).
- ADR-0539 — integer ADM HIP atomicAdd reduction pattern.
- ADR-0868 — round-1 GPU kernel coverage gap-fill (PR #351).
- ADR-0883 — round-2 HIP kernel parity coverage (PR #372).
- [docs/research/hip-kernel-coverage-round3-2026-05-31.md](../research/hip-kernel-coverage-round3-2026-05-31.md).
- Related PRs: PR #351 (round-1), PR #372 (round-2), PR #290 (HIP
  ssimulacra2 leak — round-4 blocker), PR #308 (HIP/Metal ENOSYS stubs).
- Source: `req` (operator dispatch 2026-05-31: "HIP kernel coverage round 3 — extend beyond PRs #351 + #372").
