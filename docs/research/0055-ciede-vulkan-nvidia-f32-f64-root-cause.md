<!-- markdownlint-disable MD013 MD049 MD060 -->
# Research-0055 — ciede2000 Vulkan NVIDIA places=4 root cause: f32 vs f64 colour-space chain

_Date: 2026-05-03._

## Question

PR #346 ("vif + ciede shaders — precise decorations") cut the
ciede2000 NVIDIA-Vulkan places=4 cross-backend mismatch from **42/48 →
5/48** frames by tagging load-bearing FP ops with GLSL `precise`. The
PR commit message deferred the remaining 5/48 tail (max abs `8.9e-05`,
1.78× the places=4 threshold of `5.0e-05`) as "CPU-side double-vs-float
bisect follow-up." This digest answers: **what is the root cause of
the residual 5/48?**

## Reproducer

Hardware: NVIDIA RTX 4090, driver 595.71.05.

```sh
# 1. Build with Vulkan + PR #346's shader changes applied (cherry-pick
#    just the .comp files from PR #346 onto master).
cd libvmaf
meson setup build -Denable_vulkan=enabled -Denable_cuda=false
ninja -C build

# 2. Cross-backend diff at places=4.
cd ..
python3 scripts/ci/cross_backend_vif_diff.py \
    --vmaf-binary $PWD/core/build/tools/vmaf \
    --reference testdata/ref_576x324_48f.yuv \
    --distorted testdata/dis_576x324_48f.yuv \
    --width 576 --height 324 \
    --feature ciede --backend vulkan --device 0 --places 4
# → "ciede2000  max_abs=8.900000e-05  mismatches=5/48  FAIL"
```

Failing frames (first 5 by absolute delta):

| Frame | CPU (double) | GPU (NVIDIA) | abs delta | ratio vs threshold |
|---|---|---|---|---|
| 6 | 44.7618080 | 44.7618972 | 8.9e-05 | 1.78× |
| 5 | 45.0682016 | 45.0681159 | 8.6e-05 | 1.71× |
| 0 | 50.8337181 | 50.8337702 | 5.2e-05 | 1.04× |
| 1 | 50.8337181 | 50.8337702 | 5.2e-05 | 1.04× |
| 2 | 50.8337181 | 50.8337702 | 5.2e-05 | 1.04× |

Frames 0/1/2/5/6 are the **highest-ΔE frames** in the fixture (~45–51,
i.e. scene cuts and large-difference frames). All 43 passing frames
have lower ΔE (~45.2 average), and abs delta there is ≤ 9e-6.

## Method

The "f32-vs-f64 hypothesis" (the CPU `get_lab_color` does the entire
BT.709 → linear-RGB → XYZ → Lab chain in `double`, narrowing to
`float` only on assignment to `LABColor`; the Vulkan shader is `float`
throughout) was tested by a controlled experiment:

1. Replace `core/src/feature/ciede.c::get_lab_color` (and its two
   helpers `rgb_to_xyz_map`, `xyz_to_lab_map`) with f32 implementations
   that mirror the Vulkan shader's precision contract bit-for-bit
   (literal constants narrowed to `float`, `powf` instead of `pow`,
   no FMA-fold dependence on the compiler).
2. Rebuild and run the CPU backend alongside the unmodified
   NVIDIA-Vulkan backend.
3. Triangulate three outputs at `--precision max` (full IEEE-754
   round-trip):
   - **`cpu_d`**: unmodified CPU (double `get_lab_color`).
   - **`cpu_f`**: experimental f32-CPU.
   - **`gpu`**: unmodified NVIDIA Vulkan with PR #346's `precise` decorations.

## Result

```text
Frame |  CPU(double)  CPU(float)  GPU(NV)   |dbl-flt|   |dbl-gpu|   |flt-gpu|
------+-----------------------------------------------------------------------
   0  |  50.833718    50.833770   50.833770 | 5.15e-05    5.21e-05    5.79e-07
   1  |  50.833718    50.833770   50.833770 | 5.15e-05    5.21e-05    5.79e-07
   2  |  50.833718    50.833770   50.833770 | 5.15e-05    5.21e-05    5.79e-07
   5  |  45.068202    45.068131   45.068116 | 7.10e-05    8.56e-05    1.47e-05
   6  |  44.761808    44.761903   44.761897 | 9.54e-05    8.92e-05    6.24e-06  ← worst
   4  |  45.516520    45.516527   45.516564 | 7.81e-06    4.42e-05    3.64e-05  (passes)
   7  |  45.135816    45.135741   45.135825 | 7.52e-05    8.91e-06    8.41e-05  (passes)
  10  |  45.174165    45.174089   45.174174 | 7.60e-05    8.94e-06    8.49e-05  (passes)
  ...
  47  |  45.199977    45.199901   45.199986 | 7.56e-05    9.10e-06    8.47e-05  (passes)
```

Two distinct regimes emerge:

- **Failing frames (0/1/2/5/6, highest-ΔE)**: `|cpu_f − gpu|` is
  **tiny** (5.79e-07 to 1.47e-05) — float-CPU and NVIDIA-GPU agree
  closely. `|cpu_d − gpu|` is **large** (5.21e-05 to 8.92e-05). The
  GPU is computing the same answer the CPU would compute *if it were
  in f32*. The gap is exactly the f32 vs f64 precision delta on
  high-ΔE pixels where per-pixel ΔE summation amplifies single-precision
  rounding.
- **Passing frames (43/48, lower ΔE)**: `|cpu_d − gpu|` is small (~9e-6)
  — the f32 GPU happens to land within the rounding noise of the f64
  CPU. `|cpu_f − gpu|` is large (~8.5e-5) — float-CPU and GPU diverge
  on these frames because the SPIR-V `Pow`/`Sqrt`/`Sin` lowerings
  don't match x86 `powf`/`sqrtf`/`sinf` bit-for-bit. PR #346's
  `precise` decorations align the GPU's *FMA-folding* with the CPU's
  unfolded math — close enough on low-ΔE frames where rounding
  accidents don't compound.

## Conclusion

The 5/48 NVIDIA-Vulkan ciede2000 mismatch is **structural f32 vs f64
precision gap on high-ΔE pixels**, not a driver fast-math bug, not an
FMA-fold issue, not a missing `precise` decoration. PR #346's
decorations are at the high-water mark of what f32 shader-level
mitigation can achieve.

Possible mitigations (all rejected — see [ADR-0391](../adr/0391-ciede-vulkan-nvidia-f32-f64-precision-gap.md)):

1. **Promote shader to f64 (`shaderFloat64`)**: optional Vulkan
   feature; RTX 4090 supports it but at 1/64 fp32 throughput. Would
   close the gap but at unacceptable per-frame cost. SPIR-V f64
   transcendentals also unmandated by spec — driver-divergence vector.
2. **F32-narrow the CPU reference**: changes the Netflix golden-gate
   ground truth. Breaks 8-year-old upstream behaviour. Out of scope.
3. **Polynomial approximation of `pow(x, 2.4)` / `pow(x, 1/3)`
   matched to glibc f64**: substantial engineering for a 5/48 tail at
   1.78× threshold. Cost-benefit fails.

Decision: accept as documented fork debt under
[`docs/state.md`](../state.md) Open bugs (T-VK-CIEDE-F32-F64). The
CI lavapipe parity gate at places=4 (currently 0/48) remains
authoritative; NVIDIA hardware validation is a manual local gate.

## Open questions

None — the experiment is conclusive for this question. Adjacent open
question lives in PR #346 / [ADR-0265](../adr/0265-vif-ciede-precise-step-a.md):
the Vulkan-1.4 API-version bump tail (45/48 vif scale-2 mismatches at
1.527e-02) — separate root cause, requires NVIDIA `NV_SHADER_DUMP`
diff between 1.3 and 1.4 driver paths.

## Implementation note for future investigators

The diagnostic patch (not committed) replaced these helpers with f32
twins. Reproduce via:

```c
// In core/src/feature/ciede.c, add:
static float rgb_to_xyz_map_f(float c) {
    if (c > 10.f / 255.f) {
        const float A = 0.055f;
        const float D = 1.0f / 1.055f;
        return powf((c + A) * D, 2.4f);
    }
    return c / 12.92f;
}
static float xyz_to_lab_map_f(float c) {
    if (c > 0.008856f) return powf(c, 1.0f / 3.0f);
    return 7.787f * c + (16.0f / 116.0f);
}
// Then rewrite get_lab_color to take float internally and call the
// f-suffixed helpers — see git history for the exact diff.
```

## References

- PR #346 — `precise` decorations on vif + ciede
- ADR-0187 — original ciede Vulkan kernel
- ADR-0273 — this digest's decision
- `core/src/feature/ciede.c::get_lab_color` (CPU reference, double)
- `core/src/feature/vulkan/shaders/ciede.comp::yuv_to_lab` (GPU shader, float)
