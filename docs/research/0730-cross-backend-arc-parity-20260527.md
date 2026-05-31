<!-- markdownlint-disable MD060 -->
# Research-0730: Cross-backend numerical parity — Intel Arc A380 (2026-05-27)

**Date:** 2026-05-27
**Researcher:** Claude (Anthropic) / Lusoris
**Status:** Complete — findings ready for CI promotion decision

---

## 1. Purpose

Measure per-feature numerical parity between the CPU scalar reference and
both the SYCL/oneAPI and Vulkan (Mesa ANV) GPU backends running on the
Intel Arc A380 (DG2/Alchemist, `0x56a5`). The CUDA RTX 4090 was occupied
with K150K feature extraction at the time; the Arc was the only available
idle GPU lane.

The test produces a gate-quality dataset that can inform whether the Arc
should be promoted to a required CI lane (ADR-0214 § "Arc promotion
readiness").

---

## 2. Test setup

### 2.1 Hardware

| Item | Value |
|---|---|
| GPU | Intel Arc A380 Graphics (DG2-G10, PCI 03:00.0) |
| Kernel driver | i915 (CachyOS kernel 7.0.10-1-cachyos) |
| Vulkan driver | Mesa Intel ANV 26.1.1-arch2.1 |
| Vulkan API version | 1.4.348 |
| SYCL runtime | Intel oneAPI 2025.3.2 (Level Zero 1.15.38308) |
| SYCL compiler | icpx 2025.3.2.20260112 |
| Note | Arc A380 lacks native fp64 (`shaderFloat64 = false`) |

### 2.2 Firmware / software

| Component | Version |
|---|---|
| vmaf git SHA | `d4458190a9a8ad14da8f3eabc6be4b422f9af49c` |
| Build dir | `libvmaf/build-sycl-vulkan` |
| Build flags | `enable_sycl=true`, `enable_vulkan=enabled`, `enable_cuda=false` |
| AOT SYCL targets | dg2-g10, dg2-g11, acm-g10/g11/g12, tgllp, adl-s/p/n, rpl-s/p, mtl-h/u, arl-h/s/u, lnl-m, bmg-g21/g31 |
| Vulkan device index | 1 (GPU0 = RTX 4090 at index 0) |
| SYCL device index | 0 (Level Zero; `[level_zero:gpu][level_zero:0]`) |

### 2.3 Test fixture

| Item | Value |
|---|---|
| Reference | `testdata/ref_576x324_48f.yuv` |
| Distorted | `testdata/dis_576x324_48f.yuv` |
| Resolution | 576 × 324 |
| Frames | 48 |
| Pixel format | YUV420, 8-bit |

### 2.4 Gate invocation

```bash
# SYCL matrix
python3 scripts/ci/cross_backend_parity_gate.py \
  --vmaf-binary libvmaf/build-sycl-vulkan/tools/vmaf \
  --reference testdata/ref_576x324_48f.yuv \
  --distorted testdata/dis_576x324_48f.yuv \
  --width 576 --height 324 \
  --features vif adm motion motion_v2 psnr float_moment float_ssim \
    float_ms_ssim float_psnr float_motion float_vif float_adm \
    float_ansnr psnr_hvs ciede ssimulacra2 \
  --backends cpu sycl --sycl-device 0 \
  --json-out .workingdir2/cross-backend-arc-20260527/sycl_matrix.json \
  --md-out  .workingdir2/cross-backend-arc-20260527/sycl_matrix.md

# Vulkan matrix (Intel Arc = device 1)
python3 scripts/ci/cross_backend_parity_gate.py \
  --vmaf-binary libvmaf/build-sycl-vulkan/tools/vmaf \
  --reference testdata/ref_576x324_48f.yuv \
  --distorted testdata/dis_576x324_48f.yuv \
  --width 576 --height 324 \
  --features vif adm motion motion_v2 psnr float_moment float_ssim \
    float_ms_ssim float_psnr float_motion float_vif float_adm \
    float_ansnr psnr_hvs ciede ssimulacra2 \
  --backends cpu vulkan --vulkan-device 1 \
  --json-out .workingdir2/cross-backend-arc-20260527/vulkan_matrix.json \
  --md-out  .workingdir2/cross-backend-arc-20260527/vulkan_matrix.md
```

`cambi` was excluded from the automated matrix run because the parity gate
expects the raw metric key `Cambi_feature_cambi_score` while the JSON output
uses the options-qualified alias (`cambi_encbd_8_ench_324_encw_576` for SYCL,
`cambi` for Vulkan/CPU). Cambi results were gathered separately (§3.3).

---

## 3. Results

### 3.1 SYCL backend — cpu vs sycl (Intel Arc A380, Level Zero)

| Feature | Max abs diff | Tolerance | Status | Notes |
|---|---:|---:|---|---|
| `vif` | 1.000e-06 | 5.0e-05 | **OK** | All 4 scales within places=4 |
| `adm` | 2.000e-06 | 5.0e-05 | **OK** | All 5 metrics within places=4 |
| `motion` | 3.000e-06 | 5.0e-05 | **OK** | 3-frame window mode |
| `motion_v2` | 0.000e+00 | 5.0e-05 | **OK** | Bit-exact |
| `psnr` | 0.000e+00 | 5.0e-05 | **OK** | Bit-exact |
| `float_moment` | 0.000e+00 | 5.0e-05 | **OK** | Bit-exact |
| `float_ssim` | 2.680e-04 | 5.0e-05 | **FAIL** | 40/48 frames exceed places=4 |
| `float_ms_ssim` | 1.000e-06 | 5.0e-05 | **OK** | Well within contract |
| `float_psnr` | 0.000e+00 | 5.0e-05 | **OK** | Bit-exact |
| `float_motion` | 1.300e-05 | 5.0e-05 | **OK** | |
| `float_vif` | 1.400e-05 | 5.0e-05 | **OK** | |
| `float_adm` | 4.600e-05 | 5.0e-05 | **OK** | Near the contract ceiling |
| `float_ansnr` | 1.590e-04 | 5.0e-05 | **FAIL** | 3/48 frames exceed places=4 on `float_ansnr` |
| `psnr_hvs` | 1.000e-06 | 5.0e-04 | **OK** | Well within places=3 contract |
| `ciede` | 1.550e-04 | 5.0e-03 | **OK** | Well within places=2 contract |
| `ssimulacra2` | 8.723e-02 | 5.0e-03 | **FAIL** | 38/48 frames exceed places=2; ~1 order of magnitude over |
| `cambi` | 1.487e+00 | 5.0e-05 | **FAIL** | All non-zero CPU frames return 0.0 on SYCL (see §3.3) |

**SYCL summary:** 13 OK, 4 FAIL (`float_ssim`, `float_ansnr`, `ssimulacra2`, `cambi`).

### 3.2 Vulkan backend — cpu vs vulkan (Intel Arc A380, Mesa ANV)

| Feature | Max abs diff | Tolerance | Status | Notes |
|---|---:|---:|---|---|
| `vif` | 1.000e-06 | 5.0e-05 | **OK** | |
| `adm` | 2.000e-06 | 5.0e-05 | **OK** | |
| `motion` | 3.000e-06 | 5.0e-05 | **OK** | |
| `motion_v2` | 0.000e+00 | 5.0e-05 | **OK** | Bit-exact |
| `psnr` | 0.000e+00 | 5.0e-05 | **OK** | Bit-exact |
| `float_moment` | 0.000e+00 | 5.0e-05 | **OK** | Bit-exact |
| `float_ssim` | 3.020e-04 | 5.0e-05 | **FAIL** | 1/48 frames exceed places=4 |
| `float_ms_ssim` | 1.000e-06 | 5.0e-05 | **OK** | |
| `float_psnr` | 0.000e+00 | 5.0e-05 | **OK** | Bit-exact |
| `float_motion` | 1.300e-05 | 5.0e-05 | **OK** | |
| `float_vif` | 1.500e-05 | 5.0e-05 | **OK** | |
| `float_adm` | 2.000e-05 | 5.0e-05 | **OK** | |
| `float_ansnr` | 1.590e-04 | 5.0e-05 | **FAIL** | 3/48 frames on `float_ansnr` |
| `psnr_hvs` | 2.900e-05 | 5.0e-04 | **OK** | Well within places=3 |
| `ciede` | 6.900e-05 | 5.0e-03 | **OK** | Well within places=2 |
| `ssimulacra2` | 5.482e-02 | 5.0e-03 | **FAIL** | 43/48 frames exceed places=2 |
| `cambi` | 1.487e+00 | 5.0e-05 | **FAIL** | Same as SYCL: non-zero CPU frames return 0.0 on Vulkan |

**Vulkan summary:** 13 OK, 4 FAIL (`float_ssim`, `float_ansnr`, `ssimulacra2`, `cambi`).

### 3.3 Cambi manual comparison

Cambi was probed manually because the parity gate's metric-key lookup
(`Cambi_feature_cambi_score`) does not match the alias-resolved key
the JSON output stores:

- CPU extractor: outputs key `cambi` (the registered alias)
- SYCL extractor: outputs key `cambi_encbd_8_ench_324_encw_576`
  (options-qualified alias; the `encbd`, `ench`, `encw` parameters
  encode encode-bitdepth/height/width at their defaults)
- Vulkan extractor: outputs key `cambi` (same as CPU)

All three extractors report `0.0` on every frame where the CPU reports
a non-zero score (frames 3–47 of the 48-frame fixture; max CPU value
`1.487095`). The Vulkan runtime emitted the message:
`libvmaf: Vulkan: VIF g/sv_sq using fp32 path on "Intel(R) Arc(tm) A380
Graphics (DG2)" (no shaderFloat64)` during unrelated extractors, confirming
the Arc A380 has `shaderFloat64 = false`. The cambi SYCL kernel warning
stated `device lacks native fp64 — kernels already use fp32 + int64 paths`.

The zero-score failure is not an fp64 emulation issue per se — the SYCL
build already claims to use fp32/int64 paths — but may reflect a race or
output-buffer lifetime bug in the Strategy II hybrid where the GPU
pre-process pass (preprocess, derivative, SAT mask) writes into host-mapped
buffers that the CPU residual (`vmaf_cambi_calculate_c_values +
vmaf_cambi_spatial_pooling`) reads back. The CPU residual receives zeros
after the GPU phase on Arc A380, suggesting either the DMA fence or the
`memcpy` from `h_image`/`h_mask` back into the `VmafPicture` buffers is
not completing correctly before the CPU host pass runs.

**This is a pre-existing bug, not introduced by this research run. No
feature extractor C source was modified.**

---

## 4. Failure analysis

### 4.1 `float_ssim` — places=4 violation on Arc A380

- SYCL: max_abs_diff = 2.680e-04, 40/48 frames fail.
- Vulkan: max_abs_diff = 3.020e-04, 1/48 frames fail.

`float_ssim` computes a ratio of local means, variance, and covariance via
floating-point convolutions and reductions. The Arc A380's lack of native
fp64 affects shader code that computes intermediate sums in 32-bit float
rather than 64-bit, which accumulates rounding error over the 576×324
image. The mismatch magnitude (2.7e-04) is ~5× over the places=4 contract
(5e-05). The same kernel runs at places=4 on NVIDIA RTX 4090 (Ampere) and
Intel lavapipe (software reference), which both have fp64 or use the exact
same integer accumulator path as the CPU. The GLSL shader likely falls back
to `float` for the convolution sums on this device, while the CPU path uses
`double`. This is an architecture-specific relaxation candidate: the
contract for Arc A380 would need to be places=3 (5e-04) to pass at 2.7e-04
divergence.

### 4.2 `float_ansnr` — places=4 violation

- Both SYCL and Vulkan: max_abs_diff = 1.590e-04 on `float_ansnr`,
  3/48 frames fail; `float_anpsnr` passes at max_abs_diff = 5e-06.

The 3×3 ref filter and 5×5 dis filter in `float_ansnr` involve
per-work-group float reductions and a final `log10` transform. On
an fp64-capable GPU (RTX 4090) this lands at max_abs_diff ~6e-06;
on Arc A380 the float accumulator error is ~25× larger but still
isolated to 3 frames. The existing places=3 (5e-04) contract in
ADR-0194 would comfortably accommodate this. Since the gate was run
at the default places=4 (5e-05) contract, `float_ansnr` technically
fails, but the deviation is within the intended ADR-0194 relaxed contract.
**This is not a real regression** — the gate was run at the tighter
default; the calibrated contract for this feature is places=3.

### 4.3 `ssimulacra2` — places=2 violation on Arc A380

- SYCL: max_abs_diff = 8.723e-02, 38/48 frames.
- Vulkan: max_abs_diff = 5.482e-02, 43/48 frames.

`ssimulacra2` uses XYB colour transform (cube root) and multi-scale
IIR blur reassociation — both involve significant transcendental
operations. The existing contract is places=2 (5e-03) per ADR-0192.
The observed divergence of 5–9e-02 is ~10–17× over that contract.
This indicates a systematic fp32-vs-fp64 accumulation difference in
the XYB cube root or IIR blur kernels that is amplified by Arc's lack
of native fp64. Whether this is a kernel correctness gap or a fundamental
precision limit of the Arc architecture requires further investigation
with fp32-forced CPU comparison runs.

### 4.4 `cambi` — zero-score regression

As described in §3.3: the GPU pre-process stage returns zero for all frames
where the CPU produces non-zero banding scores. Both SYCL and Vulkan are
affected identically on Arc A380. The bug is likely a DMA/barrier issue in
the Strategy II hybrid's memcpy path. This is a correctness bug in the
cambi GPU implementation, not a precision issue.

---

## 5. Comparison with other hardware

For context, the ADR-0234 calibration table records the following for
reference hardware:

- **RTX 4090 (CUDA/Vulkan/SYCL):** All features pass places=4 except
  the inherently relaxed transcendentals (ciede: places=2, psnr_hvs:
  places=3, ssimulacra2: places=2). `float_ssim` passes at places=4.
- **lavapipe (Mesa softpipe):** Used in CI as the Vulkan reference;
  all features including `float_ssim` pass at places=4.

Arc A380 is the first hardware where `float_ssim` and `ssimulacra2`
fail at their contracted tolerances. This is consistent with the
fp64-less architecture.

---

## 6. Recommendations

### 6.1 CI promotion verdict

**Not ready for `required` CI status.** The `float_ssim`, `ssimulacra2`,
and `cambi` failures block promotion. The `float_ansnr` failure is a
gate-configuration issue (ADR-0194's relaxed places=3 contract was not
applied), not a real regression.

For the Arc lane to be promoted, the following prerequisites must be met:

1. **`float_ssim` on Arc A380**: Add an Arc A380 entry to the ADR-0234
   calibration YAML with `float_ssim` tolerance set to places=3 (5e-04),
   or fix the GLSL shader to avoid fp32 accumulation divergence on
   `shaderFloat64=false` devices. Requires measurement-driven ADR.
2. **`ssimulacra2` on Arc A380**: Investigate whether the XYB/IIR
   kernels can be restructured to use integer or fp32-stable math on
   devices without fp64. Until then, add an Arc A380 calibration entry
   with `ssimulacra2` tolerance at places=1 (5e-02) at minimum, or
   mark the cell SKIP for this architecture.
3. **`cambi` GPU zero-score bug**: File a bug and fix the
   DMA/barrier issue in the SYCL/Vulkan cambi hybrid path before Arc
   can be treated as a valid cambi parity reference.

### 6.2 Arc as an `informational` CI lane

The Arc A380 is viable today as an **informational (non-blocking) CI
lane** for the 13 features that pass (`vif`, `adm`, `motion`,
`motion_v2`, `psnr`, `float_moment`, `float_ms_ssim`, `float_psnr`,
`float_motion`, `float_vif`, `float_adm`, `psnr_hvs`, `ciede`).
Adding a non-blocking workflow that runs these 13 cells and posts results
as a PR comment would give early signal on regressions without blocking
merges.

### 6.3 `float_ansnr` gate configuration

The parity gate should supply `--gpu-id arc:dg2-g10` and a corresponding
calibration-table entry once an ADR is approved. For now, noting that
`float_ansnr` at places=3 passes on Arc A380 (max_abs_diff = 1.590e-04
< 5.0e-04) avoids a false-positive block.

### 6.4 Cambi bug ticket

The cambi zero-score issue should be tracked as a bug. The symptom
(`h_image`/`h_mask` contains zeros after GPU dispatch on Arc A380)
suggests the memcpy-from-device or USM migration does not flush correctly
on the Level Zero path for this specific device. A reproducer:

```bash
LD_LIBRARY_PATH=/opt/intel/oneapi-2025.3/2025.3/lib \
  libvmaf/build-sycl-vulkan/tools/vmaf \
  --reference testdata/ref_576x324_48f.yuv \
  --distorted testdata/dis_576x324_48f.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --feature cambi_sycl --no_prediction --json --output /tmp/cambi_sycl.json \
  --backend sycl --sycl_device 0
# Expected: non-zero scores for frames 3–47
# Actual: all zeros
```

---

## 7. Raw artefacts

Stored under `.workingdir2/cross-backend-arc-20260527/` (gitignored):

- `sycl_matrix.json` — full per-cell/per-metric JSON for cpu↔sycl
- `vulkan_matrix.json` — full per-cell/per-metric JSON for cpu↔vulkan
- `sycl_matrix.md` — gate Markdown table for cpu↔sycl
- `vulkan_matrix.md` — gate Markdown table for cpu↔vulkan
- `vmaf_runs/` — per-extractor vmaf JSON output files (cpu\_\*, sycl\_\*)
- `vmaf_runs_vulkan/` — per-extractor vmaf JSON output files (cpu\_\*, vulkan\_\*)

---

## 8. References

- ADR-0214: T6-8 GPU-parity CI gate
- ADR-0234: Cross-backend calibration table
- ADR-0192/ADR-0200: `ssimulacra2` Vulkan kernel and precision contract
- ADR-0194: `float_ansnr` precision contract (places=3)
- ADR-0188/ADR-0189: `float_ssim` GPU kernel and places=4 contract
- ADR-0360: Cambi GPU (Strategy II hybrid) — SYCL twin
- `scripts/ci/cross_backend_parity_gate.py`
- `scripts/ci/gpu_ulp_calibration.yaml`
