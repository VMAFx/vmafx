<!-- markdownlint-disable MD013 MD060 -->
# Research Digest — CUDA kernel parity coverage round 3 (2026-05-31)

**Companion to**: [ADR-0947](../adr/0947-cuda-kernel-coverage-round3.md)
**Builds on**: [ADR-0868](../adr/0868-gpu-backend-kernel-coverage.md) (round 1),
[ADR-0886](../adr/0886-cuda-kernel-coverage-round2.md) (round 2),
[ADR-0214](../adr/0214-cross-backend-parity-gate.md) (places=4 / 1e-4 gate)

## Context

The 2026-05-30 audit catalogued 18 CUDA feature extractors under
`core/src/feature/cuda/`. Round 1 (PR #351) added parity gates for
2 kernels (`psnr_cuda`, `ciede_cuda`); round 2 (PR #374, in flight)
queues 5 more (`adm_cuda`, `motion_v2_cuda`, `cambi_cuda`,
`psnr_hvs_cuda`, `integer_ssim_cuda`). Round 3 closes the remaining
high-impact gap: the **float-path twins** and the new
`ssimulacra2_cuda` kernel.

## Kernel selection

### Enumeration (origin/master, 2026-05-31)

| # | Kernel (CUDA) | CPU twin | Status before R3 |
|---|---|---|---|
| 1 | `integer_motion_cuda` (motion3) | `integer_motion` | covered (test_cuda_motion3_parity.c) |
| 2 | `integer_vif_cuda` | `integer_vif` | covered (ADR-0541) |
| 3 | `psnr_cuda` | `psnr` (integer) | covered by PR #351 (round 1) |
| 4 | `ciede_cuda` | `ciede` | covered by PR #351 (round 1) |
| 5 | `adm_cuda` (integer) | `adm` | covered by PR #374 (round 2) |
| 6 | `motion_v2_cuda` | `motion_v2` | covered by PR #374 (round 2) |
| 7 | `cambi_cuda` | `cambi` | covered by PR #374 (round 2) |
| 8 | `psnr_hvs_cuda` | `psnr_hvs` | covered by PR #374 (round 2) |
| 9 | `integer_ssim_cuda` | `integer_ssim` | covered by PR #374 (round 2) |
| 10 | `float_psnr_cuda` | `float_psnr` | **R3 picks** |
| 11 | `float_vif_cuda` | `float_vif` | **R3 picks** |
| 12 | `float_ms_ssim_cuda` | `float_ms_ssim` | **R3 picks** |
| 13 | `float_moment_cuda` | `float_moment` | **R3 picks** |
| 14 | `ssimulacra2_cuda` | `ssimulacra2` | **R3 picks** |
| 15 | `float_adm_cuda` | `float_adm` | deferred (overlaps R2 integer `adm_cuda`) |
| 16 | `float_motion_cuda` | `float_motion` | deferred (overlaps R2 `motion_v2_cuda` blend) |
| 17 | `speed_chroma_cuda` | `speed_chroma` | deferred (host-side 25×25 eigendecomp, ADR-0567) |
| 18 | `speed_temporal_cuda` | `speed_temporal` | deferred (same host-side path) |

### Why these 5

- **float_psnr / float_vif / float_ms_ssim / float_moment** form the
  float-path lineage that `vmaf_float_v0.6.1` and every research-time
  `--feature float_*` invocation exercises. The integer-path twins are
  already gated; without float gates, a SIMD pivot on either backend
  could silently drift the float scores away from CPU reference.
- **ssimulacra2_cuda** is the newest CUDA kernel on the fork (per
  `changelog.d/added/0067-ssimulacra2-cuda-leaks-perf.md`); its
  Mul / Blur sub-kernels see rapid iteration and have the highest
  silent-drift risk on the backlog.

### Why the deferrals

- **float_adm_cuda** computes the same DWT2 / CSF / decouple / CM
  pipeline as integer `adm_cuda` covered by PR #374 — adding a float
  twin here would double the meson.build conflict surface against
  in-flight PR #374 without proportional coverage gain. Defer to a
  follow-up ADR after PR #374 merges.
- **float_motion_cuda** uses the same motion-blend formula as
  `motion_v2_cuda` (PR #374); overlap analysis at `core/src/feature/cuda/
  float_motion_cuda.c:300-360` confirms the blend post-process is
  shared. Defer to the follow-up that owns the blend gate.
- **speed_chroma_cuda / speed_temporal_cuda** run a 25×25 covariance
  eigendecomposition on host (ADR-0567 — unavoidable serial
  constraint). The cross-backend tolerance budget for an
  eigendecomp-bearing path is not 1e-4; needs its own ADR with a
  separately-derived tolerance.

## Test design

### Fixture

- **Geometry**: 256×144 YUV420P 8-bpc — large enough for every kernel's
  smallest scale (`float_vif` 4-scale pyramid → 32×18 at scale 3;
  `float_ms_ssim` 5-scale → 16×9 at scale 4; `ssimulacra2` 6-scale →
  ~8×4 at the bottom).
- **Pattern**: deterministic ramp; frame-dependent offset so
  successive frames differ. Distorted frame adds a low-amplitude
  pseudo-random offset (~ mod 9-17 range) so PSNR sits ~30-40 dB
  (finite, non-trivial). Chroma planes carry distinct ramps in the
  ssimulacra2 fixture (which reads chroma via YUV→XYB) and uniform
  128 elsewhere (luma-only metrics).
- **Frames**: 3 (read score at index 1 to exercise both initial and
  steady-state paths).

### Tolerance

ADR-0214 places=4 (1e-4) — matches every other CPU-vs-CUDA parity
gate on the fork.

### Skip behaviour

`vmaf_cuda_state_init` returns non-zero → `[skip: no CUDA device]`
emitted to stderr, test returns success. Mirrors
`test_cuda_motion3_parity.c` / `test_cuda_buffer_alloc_oom.c`.

## PR overlap audit

| PR | Touches | Conflict surface vs. R3 |
|---|---|---|
| PR #289 (CUDA PTX unload) | `core/src/cuda/*.c` runtime — adds no test files | None |
| PR #351 (round 1) | adds `test_cuda_psnr_parity.c` + `test_cuda_ciede_parity.c` + meson.build edits | meson.build at the same insertion point — trivial three-way merge (R3 picks distinct kernels, test files don't collide) |
| PR #374 (round 2) | adds `test_cuda_adm_parity.c` etc. + meson.build edits | meson.build at the same insertion point — same trivial three-way merge story |

Worst case: meson.build needs a one-time rebase after PR #351 + #374
merge; no source-file collisions because each round picks disjoint
kernels.

## Coverage delta

- Pre-R3 (origin/master): 3 of 18 CUDA kernels gated (~17 %)
- Post-R3 standalone: 8 of 18 (~44 %)
- Post-R1+R2+R3 (all three rounds): 13 of 18 (~72 %)

Remaining 5 = `float_adm_cuda`, `float_motion_cuda`,
`speed_chroma_cuda`, `speed_temporal_cuda`, plus
`integer_motion_cuda`'s `motion_v2` companion already covered.

## Reproducer

```bash
# Container build per CLAUDE.md §15
docker exec vmaf-dev-mcp bash -lc '
  meson setup /tmp/cuda-r3 /workspace/core \
    -Denable_cuda=true -Denable_sycl=false && \
  ninja -C /tmp/cuda-r3 \
    test/test_cuda_float_psnr_parity \
    test/test_cuda_float_vif_parity \
    test/test_cuda_float_ms_ssim_parity \
    test/test_cuda_float_moment_parity \
    test/test_cuda_ssimulacra2_parity && \
  meson test -C /tmp/cuda-r3 --suite=fast \
    test_cuda_float_psnr_parity \
    test_cuda_float_vif_parity \
    test_cuda_float_ms_ssim_parity \
    test_cuda_float_moment_parity \
    test_cuda_ssimulacra2_parity
'
```

On a CPU-only runner all five tests print `[skip: no CUDA device]`
and pass; on a CUDA-enabled runner all five assert agreement at
1e-4.

## References

- ADR-0947 — this round's decision record
- ADR-0868 — round 1 (`psnr_cuda`, `ciede_cuda`)
- ADR-0886 — round 2 (`adm_cuda`, `motion_v2_cuda`, `cambi_cuda`, `psnr_hvs_cuda`, `integer_ssim_cuda`)
- ADR-0214 — cross-backend parity tolerance gate
- ADR-0541 — integer_vif CPU-vs-CUDA parity (reference template)
- ADR-0567 — speed_chroma host-side eigendecomp (deferral rationale)
- `docs/research/gpu-backend-kernel-coverage-audit-2026-05-30.md` — round 1 audit
- Source: req (CUDA kernel coverage round 3 — extend beyond PRs #351 + #374)
