---
paths:
  - core/src/feature/hip/integer_ms_ssim_hip.c
  - core/src/feature/hip/integer_ms_ssim_hip.h
  - core/src/feature/hip/integer_ms_ssim/ms_ssim_score.hip
invariant: MS-SSIM vertical LCS terms must be double, clip_db is treated as ceiling, and enable_chroma scores every plane.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# ms_ssim_vert_lcs kernel and host terms must both be `double` (ADR-1071)

`ms_ssim_score.hip` kernel's `ms_ssim_vert_lcs` function and host
extractor `integer_ms_ssim_hip.c` share pair of allocation /
DtoH-copy contracts that are **rebase-sensitive**: one side updated
without other -> allocation sizes mismatch, HIP runtime silently
writes `float` values into `double`-sized buffer (or vice versa),
producing numerical garbage.

**Invariant:**

1. Kernel (`ms_ssim_vert_lcs`) writes `double *terms` — three planes
   `[l | c | s]`, one `double` per window, raster order (see frame-sum section
   below; was one `double` per HIP block until 2026-10-02).
2. Host allocates device `terms[i]` and pinned-host `h_terms[i]` of every
   plane (`MsSsimPlaneHip`) as `3 * scale_windows[i] * sizeof(double)` per
   scale (`ms_ssim_terms_bytes()`), all 5 MS-SSIM scales.
3. DtoH copy: `hipMemcpyAsync(..., ms_ssim_terms_bytes(s, i), ...)`.
4. Host accumulator: `double *h_terms[MS_SSIM_SCALES]`.
5. `c1`, `c2`, `c3` = `double` in both kernel params and `MsSsimStateHip`.

Established by ADR-1071 as direct port of CUDA ADR-0990 fix. Future
refactor reverting any of these to `float` -> cross-backend parity
regresses by ~0.004 per MS-SSIM scale (failing ADR-0214 places=4
gate on AMD hardware).

**`enable_db` / `clip_db` options** also wired into HIP extractor's
`options[]` array and `collect_fex_hip()`. Do not remove them —
required for parity with CPU (`float_ms_ssim`) and CUDA
(`float_ms_ssim_cuda`) paths.

## MS-SSIM clip_db is a dB ceiling (ADR-1221)

`float_ms_ssim.c` derives
`max_db = ceil(10 * log10(peak * peak / mse))` with
`mse = 0.5 / (w * h)` at `init()`; `convert_to_db()`
returns `MIN(-10*log10(1 - score), max_db)`, short-circuiting to
`max_db` when `score >= 1.0`. Until ADR-1221, `integer_ms_ssim_hip.c`
clamped LINEAR score into `[0, 1]`, converted with no ceiling, had no
`max_db` field: identical reference/distorted pair returned `+Inf`;
every high-similarity pair returned uncapped dB value.

`max_db` derived once in `init_fex_hip` right after
`ms_ssim_hip_init_dims()`, using CPU's exact expression and integer
types; dB conversion goes through `ms_ssim_convert_to_db()`. Guard =
`test_hip_ms_ssim_parity.c::test_ms_ssim_clip_db_ceiling`, which
feeds IDENTICAL pair — on merely high-similarity fixture, ceiling
never binds, variant passes against unfixed twin.

## enable_chroma = CPU's per-plane pipeline (2026-10-03)

`T-MS-SSIM-GPU-CHROMA-OPTION-DRIFT-2026-09-06`. Was: option accepted,
`n_planes = 1u` on both sides of dead branch, `_cb` / `_cr` silently missing
(wrong output, not fallback). Now, rebase-sensitive:

- Option table = `float_ms_ssim.c`'s four; option kept (HISS-14), never
  removed. `provided_features` = `float_ms_ssim`, `_cb`, `_cr`.
- Per plane (`MsSsimPlaneHip`): own dimensions, scale geometry, pyramid,
  pinned level 0 (`h_ref` / `h_cmp`, uploaded straight into pyramid level 0),
  terms and pinned terms. Shared: five horizontal planes (luma-sized, one
  stream). Kernels, `ms_ssim_hip_scale_sums()` and `ms_ssim_arith.h` =
  luma path; no chroma copy.
- Plane count and chroma size from `../metal/float_ms_ssim_option_semantics.h`
  (`vmaf_metal_ms_ssim_active_planes()`, `_plane_dimensions()`): YUV400P =
  luma only; every scored plane >= 176 px (4:2:0 needs 351x351 luma) or init
  refuses with CPU's message.
- Every plane validated and prepared before first emit; chroma uses
  luma's `max_db`; `enable_lcs` = luma means only, as CPU.
- Guards: `test_hip_ms_ssim_parity` (chroma `==` on 4:2:0 odd size, 4:2:2 10
  bit, 4:4:4; refusal and YUV400P verdicts; fails on old code with
  missing key), `test_hip_exact_twins` (chroma row),
  `test_hip_twin_option_parity` (option table),
  `test_hip_kernel_source_contract.py` (planted `n_planes = 1u`, luma-only
  `provided_features`), gate cell `float_ms_ssim_chroma`
  (`exact_twins.d/float_ms_ssim_chroma.hip`).
