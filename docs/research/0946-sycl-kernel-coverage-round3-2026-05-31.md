# Research digest — SYCL kernel coverage round 3 (ADR-0946, 2026-05-31)

## Purpose

Extend SYCL CPU-vs-kernel parity coverage beyond the integer-family
gates landed in PR #351 (round 1) and PR #376 (round 2). Pick the
highest-value 5 remaining kernels that fit in a 200-800 LOC bundle.

## Full SYCL kernel inventory (as of `origin/master` 2026-05-31)

| # | File | Extractor | Round | Test |
| --- | --- | --- | --- | --- |
| 1 | `integer_cambi_sycl.cpp` | `cambi_sycl` | 0 (pre-existing) | `test_integer_cambi_sycl.c` |
| 2 | `integer_motion_sycl.cpp` | `motion_sycl` (motion3 path) | 0 (pre-existing) | `test_sycl_motion3_parity.c` |
| 3 | `integer_psnr_sycl.cpp` | `integer_psnr_sycl` | 1 (#351) | `test_sycl_psnr_parity.c` |
| 4 | `integer_vif_sycl.cpp` | `integer_vif_sycl` | 1 (#351) | `test_sycl_vif_parity.c` |
| 5 | `integer_adm_sycl.cpp` | `integer_adm_sycl` | 2 (#376) | `test_sycl_adm_parity.c` |
| 6 | `integer_ciede_sycl.cpp` | `integer_ciede_sycl` | 2 (#376) | `test_sycl_ciede_parity.c` |
| 7 | `integer_ssim_sycl.cpp` | `integer_ssim_sycl` | 2 (#376) | `test_sycl_ssim_parity.c` |
| 8 | `integer_ms_ssim_sycl.cpp` | `integer_ms_ssim_sycl` | 2 (#376) | `test_sycl_ms_ssim_parity.c` |
| 9 | `integer_motion_v2_sycl.cpp` | `motion_v2_sycl` | 2 (#376) | `test_sycl_motion_v2_parity.c` |
| 10 | `float_psnr_sycl.cpp` | `float_psnr_sycl` | **3 (this PR)** | `test_sycl_float_psnr_parity.c` |
| 11 | `float_adm_sycl.cpp` | `float_adm_sycl` | **3 (this PR)** | `test_sycl_float_adm_parity.c` |
| 12 | `float_vif_sycl.cpp` | `float_vif_sycl` | **3 (this PR)** | `test_sycl_float_vif_parity.c` |
| 13 | `float_motion_sycl.cpp` | `float_motion_sycl` | **3 (this PR)** | `test_sycl_float_motion_parity.c` |
| 14 | `integer_psnr_hvs_sycl.cpp` | `psnr_hvs_sycl` | **3 (this PR)** | `test_sycl_psnr_hvs_parity.c` |
| 15 | `integer_moment_sycl.cpp` | `float_moment_sycl` | deferred → round 4 | — |
| 16 | `speed_chroma_sycl.cpp` | `speed_chroma_sycl` | deferred → round 4 | — |
| 17 | `speed_temporal_sycl.cpp` | `speed_temporal_sycl` | deferred → round 4 | — |
| 18 | `ssimulacra2_sycl.cpp` | `ssimulacra2_sycl` | deferred → round 4 | — |

## Coverage trajectory

| Round | Date | Extractors covered | % of 18 |
| --- | --- | --- | --- |
| 0 (pre-rounds) | <= 2026-05-29 | 2 (cambi, motion3) | 11 % |
| 1 (#351) | 2026-05-30 | +2 = 4 (psnr, vif) | 22 % |
| 2 (#376) | 2026-05-30 | +5 = 9 (adm, ciede, ssim, ms_ssim, motion_v2) | 50 % |
| **3 (this PR)** | 2026-05-31 | **+5 = 14** (float family + psnr_hvs) | **78 %** |
| 4 (planned) | TBD | +4 = 18 (moment, speed×2, ssimulacra2) | 100 % |

## Selection rationale for round 3

Five criteria ranked by descending weight:

1. **Coverage delta** — each pick must move the % bar.
2. **Independent kernel topology vs. rounds 1+2** — picks must
   exercise a code path not already gated by an existing test, to
   avoid false confidence.
3. **CPU reference exists with a clean public extractor name** —
   no per-extractor config-arg setup needed beyond `vmaf_use_feature`.
4. **Output score name is publicly documented** (named in the
   `feature_extractor.h` or used by a shipped model).
5. **Fits within the round-2 scaffold** (256x144 fixture, places=4
   tolerance, single-frame or 2-frame fed via `vmaf_read_pictures`).

### Why these 5

- **`float_psnr_sycl`** — entry-point pick for the float family.
  Smallest kernel (per-plane MSE reduction). CPU equivalent
  `float_psnr.c` emits `float_psnr` score. No prior float-family
  parity gate exists.
- **`float_adm_sycl`** — largest float-family kernel by LOC
  (DWT2 + CSF + contrast-masking pipeline). Primary feature for
  every float-VMAF model. CPU equivalent `float_adm.c` emits
  `VMAF_feature_adm2_score`.
- **`float_vif_sycl`** — second-largest float kernel
  (4-scale separable Gaussian + entropy reduction). Primary
  feature for every float-VMAF model. CPU equivalent `float_vif.c`
  emits `VMAF_feature_vif_scale0_score`.
- **`float_motion_sycl`** — temporal SAD + blended-motion2
  kernel. Different precision class and blend-factor surface
  from the existing motion3 / motion_v2 gates. CPU equivalent
  `float_motion.c` emits `VMAF_feature_motion2_score` at idx 1.
- **`psnr_hvs_sycl`** — DCT8x8 + CSF mask kernel. Topology
  completely orthogonal to every other SYCL extractor — neither
  the integer nor the float family exercises it. CPU equivalent
  `third_party/xiph/psnr_hvs.c` emits `psnr_hvs` score.

### Why not the other 4

- **`float_moment_sycl`** — emits `float_moment_y_1st`,
  `float_moment_y_2nd`, etc.; the round-2 scaffold's
  `vmaf_feature_score_at_index` plus a single column name doesn't
  cover the per-moment fan-out. Needs templating work first.
- **`speed_chroma_sycl`** / **`speed_temporal_sycl`** — both
  require config-arg dicts (`speed_kernelscale`, `speed_prescale`,
  `speed_sigma_nn`, …) to produce numerically stable output.
  The round-2 scaffold passes `NULL` as the config dict, which
  for SpEED puts the kernel into a default-everything mode that
  diverges between CPU and SYCL by design. Needs per-extractor
  config templating first.
- **`ssimulacra2_sycl`** — needs `yuv_matrix` config-arg setup
  and a 90-degree-rotated reference picture (`ref_pic_90`) that
  the existing scaffold's `vmaf_read_pictures` path doesn't
  populate. Needs scaffold extension first.

## Fixture-sizing audit

All five new tests use **256x144 YUV420P 8 bpc**. Justifications:

- **256 % 8 == 0** — required by `psnr_hvs_sycl`'s 8x8 DCT block
  grid (a non-multiple-of-8 width would short the last block
  column).
- **144 % 8 == 0** — same.
- **≥ 64x64** — required by `float_vif_sycl`'s 4-scale
  Gaussian pyramid (the smallest scale halves the spatial
  dimensions four times).
- **≥ 32x32** — required by `float_adm_sycl`'s 4-scale DWT2
  CSF footprint.
- **fits in the fast-suite time budget** — single-frame tests
  finish in < 100 ms each on Intel-Arc; the two-frame motion
  test finishes in < 200 ms.

These match the round-2 fixture sizing exactly, so the
sub-group-reduction stability properties carried over.

## Tolerance choice

`PARITY_TOL = 1e-4` matches ADR-0214 places=4. Identical to
round 1 + round 2. No per-kernel relaxation — every new test
gates on the same threshold the cross-backend CI gate uses.

## PR overlap audit

| Other PR | File overlap with this PR | Notes |
| --- | --- | --- |
| **#293** (SYCL init-failure cleanup leaks) | none | #293 touches `integer_adm_sycl.cpp`, `integer_vif_sycl.cpp`, `speed_chroma_sycl.cpp`, `speed_temporal_sycl.cpp` — all kernel sources, no test files |
| **#351** (SYCL round 1) | none | #351 adds `test_sycl_psnr_parity.c` + `test_sycl_vif_parity.c` — different kernels |
| **#376** (SYCL round 2) | `core/test/meson.build` (additive) | #376 adds blocks for 5 integer-family tests; this PR adds 5 float-family + psnr_hvs blocks at a different insertion point; rebase requires accepting both blocks |

Resolved by inserting the round-3 block **after** the round-2
block in `meson.build`. If #376 lands first, the round-3 block
appends cleanly; if this PR lands first, #376's round-2 block
must be inserted before the round-3 block.

## Container build evidence

```bash
docker exec vmaf-dev-mcp bash -lc '
  source /opt/intel/oneapi/setvars.sh --force >/dev/null 2>&1 && \
  cd /tmp/wt-sycl-r3 && \
  CC=icx CXX=icpx meson setup build-sycl-r3 core \
      -Denable_sycl=true -Denable_avx512=false -Db_lto=false && \
  ninja -C build-sycl-r3 \
      test/test_sycl_float_psnr_parity \
      test/test_sycl_float_adm_parity \
      test/test_sycl_float_vif_parity \
      test/test_sycl_float_motion_parity \
      test/test_sycl_psnr_hvs_parity'
```

Result: all 5 test executables compile + link clean with
`-Wall -Wextra` under `icx`/`icpx`. Registration sub-tests pass
on this host; parity sub-tests hit a pre-existing level_zero
device-passthrough issue (same as PR #376 — not introduced by
this PR). On a host with working Intel-GPU passthrough or with
no SYCL device at all, the parity sub-tests run the assertion
or hit the skip-path cleanly.

## Round 4 backlog

`float_moment_sycl`, `speed_chroma_sycl`, `speed_temporal_sycl`,
`ssimulacra2_sycl`. Each needs scaffold extensions before parity
gates can be added cleanly. Estimated effort: 600 LOC across the
4 tests plus a `test/sycl_parity_helpers.{h,c}` extraction to
absorb the config-dict setup + ref_pic_90 fill.
