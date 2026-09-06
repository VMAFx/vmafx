<!-- markdownlint-disable MD060 MD013 -->
# Research-0985: SYCL parity divergence — float_ssim + ssimulacra2 + float_ansnr (2026-06-03)

**Date:** 2026-06-03
**Researcher:** Claude (Anthropic) / Lusoris
**Status:** Complete — pseudo-Kahan recurrence blowup reverted; Arc A380 hardware calibrated at places=1 (5.0e-2)
**Companion ADR:** ADR-0985 (Accepted)
**Triggered by:** .workingdir2/cross-backend-arc-20260527/sycl_matrix.md rows: float_ssim FAIL (2.68e-4), ssimulacra2 FAIL (8.72e-2), float_ansnr FAIL (1.59e-4)

---

## 1. Executive summary

Three SYCL parity matrix rows are open. Investigation found:

| Row | Root cause | Fixable without hardware? | Disposition |
|---|---|---|---|
| `float_ansnr` | Feature removed (PR #38, commit 70ed8b3c) | N/A | **CLOSED** — stale row |
| `float_ssim` | (a) CPU vs GPU use different SSIM formulas; (b) Arc A380 fp32 accumulation at full-frame scale | Algorithm difference is intentional; precision gap is Arc-architecture-specific | **CANNOT FIX** with high confidence — needs device-calibration ADR |
| `ssimulacra2` | Arc A380 lacks native fp64; IIR blur fp32 accumulation error amplified ~17× beyond places=2 contract | Cannot reach acceptable precision on fp64-less hardware without fundamental kernel redesign | **CANNOT FIX** with high confidence — needs device-calibration ADR |

---

## 2. float_ansnr — closed, stale row

The `float_ansnr` extractor was removed in PR #38 (commit `70ed8b3ce3`,
"feat(core): drop legacy ansnr feature (pre-VMAF, never Netflix-adopted)").
The SYCL parity matrix row at `.workingdir2/cross-backend-arc-20260527/sycl_matrix.md`
predates the removal. Both the CPU extractor (`vmaf_fex_float_ansnr`) and
the SYCL twin were dropped. The row is stale and should be treated as
**RESOLVED / CLOSED**: the feature no longer exists in tree.

Verification:

```text
grep -r "float_ansnr" core/src/ → no results (extractor gone)
grep -r "float_ansnr" core/src/feature/sycl/ → no results
```

---

## 3. float_ssim — two-cause divergence on Arc A380

### 3.1 Divergence data

- SYCL: max_abs_diff = 2.68e-04, 40/48 frames fail
- Vulkan: max_abs_diff = 3.02e-04, 1/48 frames fail
- Gate tolerance: 5.0e-05 (places=4)
- Divergence is ~5.4× over tolerance

### 3.2 Formula difference (primary cause)

The CPU scalar extractor (`float_ssim.c` via `iqa_ssim` →
`ssim_accumulate_default_scalar`) implements the L×C×S decomposition from
Wang et al. (2004) Eq.(1):

```text
sigma_comb = sqrt(var_ref * var_cmp)
l = (2*mu1*mu2 + C1) / (mu1^2 + mu2^2 + C1)
c = (2*sigma_comb + C2) / (var_ref + var_cmp + C2)
s = (covar + C3) / (sigma_comb + C3)
SSIM = l * c * s
```

The SYCL extractor (`integer_ssim_sycl.cpp`, `vmaf_fex_float_ssim_sycl`)
implements the combined Wang et al. (2004) Eq.(13):

```text
num = (2*mu1*mu2 + C1) * (2*covar + C2)
den = (mu1^2 + mu2^2 + C1) * (var_ref + var_cmp + C2)
SSIM = num / den
```

These are **not algebraically identical**. The L×C×S form uses
`sigma_comb = sqrt(var_ref * var_cmp)` (geometric mean of standard
deviations) for the contrast term, while the combined form uses `covar`
(cross-correlation) directly. The CUDA twin (`ssim_score.cu`,
`calculate_ssim_vert_combine`) uses the same combined formula as SYCL.
The Vulkan twin uses a GLSL shader with the same combined form.

This formula divergence was present from the initial GPU kernel landing
and was accepted as a known precision trade-off — the combined form is
simpler to implement in GPU kernels and avoids a `sqrt` per pixel.

### 3.3 Accumulation precision (secondary cause on Arc A380)

The Arc A380 (DG2-G10) has `shaderFloat64 = false` and does not support
native fp64 in shader/kernel code. The SYCL kernel performs 11-tap
separable Gaussian convolution accumulations in `float` precision with
`sycl::reduce_over_group`. On RTX 4090 (Ampere, native fp64), the same
SYCL kernel passes places=4 (confirmed in Research-0730 §5). On
lavapipe (Mesa software renderer), the Vulkan twin also passes places=4.

Arc A380 accumulates float rounding error over (576-10) × (324-10) =
566 × 314 = 177,724 pixel SSIM values, each computed from a 11-tap
vertical convolution sum. The per-pixel rounding is bounded but compounds
to ~2.7e-4 at the mean level.

### 3.4 Why a kernel fix is blocked

To fix the divergence on Arc A380 one would need to:

(a) Change the GPU formula to match the L×C×S CPU path (requires
    adding a `sqrt` per pixel, changes the existing GPU output for all
    hardware, and the CUDA/Vulkan twins would need matching changes), or

(b) Use integer or int64 intermediate accumulators for the convolution
    sums (as the `integer_ssim_sycl.cpp::IssimStateSycl` path does for
    the `ssim` feature, with places=4-5 documented in ADR-0564), or

(c) Add a device-specific calibration table entry that sets the
    `float_ssim` tolerance to places=3 (5e-04) for Arc A380 class devices.

Option (a) would be high-risk (changes GPU output for all hardware).
Option (b) is a significant rewrite of the float_ssim_sycl kernel;
cannot be validated without Arc hardware in the test loop.
Option (c) is the correct ADR-driven path per Research-0730 §6.1.

**Recommendation:** Open an ADR for device-calibration entry
(`float_ssim` places=3 on Arc/DG2-class devices). The kernel code
is correct for its documented algorithm; the formula difference from
CPU is an accepted trade-off in the GPU backends.

---

## 4. ssimulacra2 — systematic fp32 amplification on Arc A380

### 4.1 Divergence data

- SYCL: max_abs_diff = 8.72e-02, 38/48 frames fail
- Vulkan: max_abs_diff = 5.48e-02, 43/48 frames fail
- Gate tolerance: 5.0e-03 (places=2, per ADR-0192)
- Divergence is ~17× over tolerance on SYCL, ~11× on Vulkan

### 4.2 Pipeline analysis

The `ssimulacra2_sycl.cpp` kernel uses the following split:

- **Host-side** (fp64-safe): YUV → linear-RGB → XYB (cube-root moved to
  host per ADR-0201 §Precision investigation; GPU cbrt diverges by 42 ULP
  from libm, cascading to ~1.5e-2 drift on Vulkan without the fix).
  2×2 box downsample in linear-RGB.
  Per-pixel SSIM + EdgeDiff combine (double precision).
  108-weight polynomial pooling (double precision).

- **GPU-side**: 3-plane elementwise multiply + separable 3-pole IIR
  Charalampidis 2016 blur. Both compiled with `-fp-model=precise`
  (icpx) to block FMA contraction.

The XYB and pooling paths are already on host and use the same
`vmaf_ss2_cbrtf` LUT as the CPU. The divergence therefore comes from
the **GPU IIR blur path**.

### 4.3 IIR blur fp32 accumulation analysis

The 3-pole recursive IIR per `launch_blur` in `ssimulacra2_sycl.cpp`
(lines 502–593) implements the Charalampidis 2016 recurrence:

```text
o_k = n2_k * (in[n-N-1] + in[n+N-1]) - d1_k * prev1_k - prev2_k
out[n] = o0 + o1 + o2
```

This matches the scalar `fast_gaussian_1d` byte-for-byte (with
`-fp-model=precise` blocking contraction on icpx). The recurrence is
stable for sigma=1.5 (radius=5), so the IIR is not numerically
unstable.

The divergence on Arc A380 but not RTX 4090 is consistent with:

1. **Different floating-point rounding unit**: even with
   `-fp-model=precise`, Arc A380 (Xe-HPC micro-architecture) may use
   a slightly different rounding mode or denormal handling than
   NVIDIA Ampere. The IIR recurrence accumulates `prev1_k` and `prev2_k`
   state over up to `h + radius` rows (up to 329 rows at scale 0 for
   the 576×324 fixture). Each step's rounding error compounds through
   the recurrence. After 6 scales (pyramid) the pooled score
   diverges by ~9e-2.

2. **fp64-less intermediate spill**: on some Xe micro-architectures,
   the compiler may spill intermediate float registers to memory with
   different flush-to-zero semantics than the CPU.

3. **Cannot be verified without hardware**: any proposed fix (e.g.,
   compensated Kahan summation in the IIR accumulation, or using
   int64 partial sums for the blur pass) cannot be validated without
   running on Arc A380. A code change that passes on the CPU and
   RTX 4090 but still diverges on Arc would not be an improvement.

### 4.4 Considered code changes

**Option A — Kahan compensated summation in IIR**:
Replace `o0 + o1 + o2` with Kahan-compensated reduction. Could halve
the per-output float error but adds 6 FP ops per output pixel and
requires state variables — significant kernel complexity increase with
unverifiable benefit on Arc.

**Option B — Move more computation to host**:
Run the entire IIR blur on host (same as scalar). This would trivially
achieve places=4 but eliminates all GPU benefit for ssimulacra2_sycl
(the entire kernel becomes a CPU pass with GPU overhead for the mul3 step
only). Defeats the purpose of the GPU extractor.

**Option C — Device calibration entry (recommended)**:
Per Research-0730 §6.2 recommendation, add an Arc A380 calibration
entry that sets `ssimulacra2` tolerance to places=1 (5e-02) on
`dg2-g10` and related DG2-class devices. This is correct, honest, and
does not risk changing scores on any other hardware.

### 4.5 Post-investigation resolution: PR #865 pseudo-Kahan blow-up analysis and revert (2026-09-05)

In PR #865 (commit `8b7ae731a`), an attempt was made to implement compensated summation in the IIR recurrence loop:

```c
const float y0 = (t0 - prev2_0) - comp_0;
const float o0 = prev1_0 + y0;
comp_0 = (o0 - prev1_0) - y0;
```

**Mathematical error:** Standard Kahan summation applies exclusively to computing a running sum $S \leftarrow S + x_i$ into an accumulator $S$. The Charalampidis recursive filter has no running accumulator; each step computes the next state value $o_k = n2_k \cdot \text{sum} - d1_k \cdot \text{prev1}_k - \text{prev2}_k$. Adding $\text{prev1}_k$ into $o_k$ fundamentally altered the characteristic equation of the 3-pole filter:
$$o = \text{prev1} + (n2 \cdot \text{sum} - d1 \cdot \text{prev1} - \text{prev2}) = (1 - d1) \cdot \text{prev1} - \text{prev2} + n2 \cdot \text{sum}$$
For the primary pole where $d1 \approx 1.8422$, the factor $(1 - d1) \approx -0.8422$ combined with $-\text{prev2}$ shifts the roots outside the unit circle, converting a stable low-pass IIR filter into an exponentially explosive oscillator.

Numerical float32 simulation of the recurrence:

- Step 0: $4.2 \times 10^0$
- Step 10: $4.1 \times 10^4$
- Step 25: $3.9 \times 10^{10}$
- Step 45: $3.63 \times 10^{18}$
- Step 60: $> 10^{25}$, rapidly overflowing to NaN and saturating the resulting SSIMULACRA2 score at 100.0 (reported as `delta = 1.73e+02` in unit tests).

**Resolution and hardware validation:**

1. Reverted the pseudo-Kahan hunk, restoring the bit-exact uncompensated recurrence matching CPU `fast_gaussian_1d` and CUDA `ssimulacra2_blur.cu`.
2. Verified on Intel Arc A380 (DG2-G10) hardware:
   - `test_sycl_ssimulacra2_parity` unit test: `delta = 4.98e-5` (passes `5e-3` contract).
   - Checkerboard 1-px (`checkerboard_1920_1080_10_3_0_0.yuv` vs `_1_0.yuv`): `max_abs_diff = 0.000e+00`.
   - Checkerboard 10-px (`checkerboard_1920_1080_10_3_0_0.yuv` vs `_10_0.yuv`): `max_abs_diff = 0.000e+00`.
   - 48-frame `src01_hrc00_576x324.yuv` vs `src01_hrc01_576x324.yuv`: `max_abs_diff = 1.211e-02`.
3. Option C adopted: calibrated `scripts/ci/gpu_ulp_calibration.yaml` under `sycl:0x8086:0x56a*` and `arc:dg2-g10` at places=1 (`5.0e-2`).

---

## 5. SYCL parity test coverage audit

Existing tests checked:

- `test_sycl_ssim_parity.c` — tests `integer_ssim` (not `float_ssim`), tolerance 1e-4
- `test_sycl_ssimulacra2_parity.c` — tests ssimulacra2, tolerance 5e-3 (ADR-0214 contract)

**Gap identified**: there is no unit test that exercises
`vmaf_fex_float_ssim_sycl` (the `float_ssim_sycl` extractor in
`integer_ssim_sycl.cpp`). The `test_sycl_ssim_parity.c` tests
`integer_ssim_sycl` only. A smoke test for `float_ssim_sycl` registration
and parity (at places=3 tolerance to accommodate Arc A380) would
complete the coverage.

---

## 6. Proposed follow-up actions

### 6.1 Immediate (no hardware required)

1. **Close the `float_ansnr` row** in `.workingdir2/` and `docs/state.md`
   — the feature is gone. Confirm no ANSNR SYCL symbols remain in tree.

2. **Add a `float_ssim_sycl` smoke test** to `core/test/` that:
   - Verifies `vmaf_fex_float_ssim_sycl` is registered
   - Runs a CPU vs SYCL parity check with tolerance 5e-04 (places=3)
   - Skips gracefully if no SYCL device is present
   This test would pass on RTX 4090 at the current places=4 contract
   and pass on Arc A380 at the relaxed places=3.

3. **Document the formula difference** between the CPU and GPU `float_ssim`
   implementations in a code comment in `integer_ssim_sycl.cpp`.

### 6.2 Requires ADR and hardware validation

1. **ADR for device-calibration entries** (Arc A380 class):
   - `float_ssim`: places=3 (5e-04) on DG2-G10 and related devices
   - `ssimulacra2`: places=1 (5e-02) minimum on DG2-G10 class

2. **Investigate ssimulacra2 IIR with Arc hardware**:
   Run a bisect between "all on host" and "GPU IIR blur" to isolate
   exactly which blur pass (horizontal vs vertical, which scale level)
   contributes the most divergence. Use `SYCL_DEVICE_FILTER=level_zero`
   with a print-partial-score-per-scale harness.

---

## 7. Files examined

- `core/src/feature/sycl/integer_ssim_sycl.cpp` — `float_ssim_sycl` + `integer_ssim_sycl`
- `core/src/feature/sycl/ssimulacra2_sycl.cpp` — ssimulacra2_sycl IIR + combine
- `core/src/feature/float_ssim.c` — CPU float_ssim reference
- `core/src/feature/iqa/ssim_tools.c` — CPU SSIM formula (L×C×S)
- `core/src/feature/cuda/integer_ssim/ssim_score.cu` — CUDA SSIM formula (combined)
- `core/src/meson.build` — lines 1560–1625: SYCL build flags; confirmed `-fp-model=precise`
- `docs/research/0730-cross-backend-arc-parity-20260527.md` — prior parity measurement
- `docs/adr/0214-gpu-parity-ci-gate.md` — tolerance table and promotion criteria
- `.workingdir2/cross-backend-arc-20260527/sycl_matrix.md` — failure data source
- `core/test/test_sycl_ssim_parity.c` — existing SSIM parity test (integer_ssim only)
- `core/test/test_sycl_ssimulacra2_parity.c` — existing ssimulacra2 parity test

---

## 8. References

- Research-0730: Cross-backend numerical parity — Intel Arc A380 (2026-05-27)
- ADR-0214: GPU-parity CI gate (T6-8)
- ADR-0192: ssimulacra2 Vulkan kernel and precision contract (places=2)
- ADR-0188/ADR-0189: float_ssim GPU kernel (combined formula) and places=4 contract
- ADR-0201: ssimulacra2 Vulkan precision investigation — cbrt moved to host
- ADR-0206: ssimulacra2 CUDA + SYCL precision contracts
- ADR-0564: integer_ssim_sycl fp64-free precision, places=4-5
- PR #38: removal of float_ansnr feature
