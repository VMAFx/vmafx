# ADR-0984: Port Netflix Upstream May–Jun 2026 (5 commits)

## Status

Accepted

## Date

2026-06-01

## Context

Five upstream Netflix/vmaf commits from May–June 2026 were ported to the fork,
each path-remapped from `libvmaf/` to `core/` (per ADR-0700). The commits were
applied in chronological order on branch `port/upstream-netflix-may-jun-2026`.

### Commits ported

1. **e4b93c6ed** (2026-05-08) — `tools/vmaf: enable direct read by default`
   Removes the `#ifdef USE_DIRECT_READ` compile-time guard from `vmaf.c`,
   making `video_input_fetch_into_vmaf_picture()` the only input path.
   Supersedes the compile-time flag introduced by ADR-0600.

2. **a4a1492d3** (2026-05-25) — `libvmaf: replace integer_motion with
   pipelined v2 variant, rename`
   Merges the pipelined v2 implementation into `integer_motion.c`, removes the
   separate `integer_motion_v2.c` CPU extractor, and deletes the merged
   `motion_v2_avx2.{c,h}` / `motion_v2_avx512.{c,h}` SIMD files. In this fork,
   the v2 SIMD files are retained under their original names because the GPU
   backends (CUDA, SYCL, HIP, Metal) expose `vmaf_fex_integer_motion_v2_*`
   extractors that depend on the `motion_score_pipeline_{8,16}` pipeline. The
   CPU `vmaf_fex_integer_motion_v2` extractor is removed from the
   `feature_extractor_list[]`; the GPU variants remain.

3. **c2155d6cd** (2026-06-01) — `libvmaf: add 2160p@1.5H CSF support`
   Adds `adm_ref_display_height == 2160 && adm_norm_view_dist == 1.5` branches
   to `barten_csf_tools.h`, routing them to the existing `BLENDED_CSF_1080_3H`
   tables (1.5*2160 == 3.0*1080 == 56.55 ppd). Adds pinning tests.

4. **9a078011c** (2026-06-01) — `ADM: enable adm_decouple_s123 SIMD
   dispatch and fix AVX2 kh_msb blend`
   Uncomments `s->adm_decouple_s123 = adm_decouple_s123_avx2/avx512` in
   `integer_adm.c::init()`. Fixes a kh/kv/kd MSB blend bug in the AVX2 path
   (`const_32768` → `abs_oh_epi32`) that caused small float-feature drift on
   10-bit content. Adds matching `kh_shift_epi32` zero-clear on the
   small-value branch in both AVX2 and AVX-512.

5. **30f472b14** (2026-06-01) — `Speed_chroma: vectorize
   compute_covariance (AVX2 + AVX-512)`
   Adds a `compute_cov_kernel_fn` function-pointer in `SpeedState` and
   dispatches to new `compute_cov_kernel_avx2` / `compute_cov_kernel_avx512`
   implementations in new files `x86/speed_avx2.{c,h}` and
   `x86/speed_avx512.{c,h}`. Scalar path retained as fallback.

## Decision

All 5 commits ported with path remapping. Fork-local adaptations:

- **Commit #1**: `copy_picture_data()` and `finish_unread_picture()` helper
  removed (no longer called). Call sites updated.
- **Commit #2**: CPU `vmaf_fex_integer_motion_v2` removed from
  `feature_extractor_list[]`; GPU `_v2_*` variants kept. `integer_motion_v2.c`
  and `motion_v2_avx2/avx512.{c,h}` kept for GPU build paths.
  `motion_avx2/avx512.{c,h}` updated to the pipeline implementation.
  Upstream precision reductions in `python/test/` golden assertions NOT applied
  (CLAUDE.md §12 r1 / correctness-first rule).
- **Commit #3–5**: applied cleanly.

## Alternatives considered

- **Reject commit #2 (integer_motion rename)**: the GPU backends reference the
  v2 name; a full rename would require touching all GPU files. Accepted partial
  port instead.
- **Apply upstream precision relaxations in python/test/**: rejected — they
  weaken the golden assertion gate (CLAUDE.md §12 r1).

## Consequences

- `tools/vmaf` CLI always uses direct-read input path
  (no `USE_DIRECT_READ` flag).
- `vmaf_fex_integer_motion` is now the pipelined implementation on CPU.
- 2160p at 1.5H viewing distance is a valid CSF configuration.
- ADM `adm_decouple_s123` SIMD dispatch is enabled; AVX2 kh_msb bug fixed.
- Speed_chroma `compute_covariance` is SIMD-accelerated on x86.

## References

- Upstream commits: e4b93c6ed, a4a1492d3, c2155d6cd, 9a078011c, 30f472b14
- ADR-0600: previous `USE_DIRECT_READ` port (now superseded for CPU path)
- ADR-0138, ADR-0139: SIMD bit-exactness invariants
- ADR-0700: `libvmaf/` → `core/` path remap
- CLAUDE.md §12 r1: never modify Netflix golden assertions
