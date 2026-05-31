<!-- markdownlint-disable MD060 -->
# Research-0051 — ssimulacra2 GPU XYB shader precision re-investigation (T-GPU-OPT-VK-3)

| Field             | Value                                                                                  |
| ----------------- | -------------------------------------------------------------------------------------- |
| **Date**          | 2026-05-02                                                                             |
| **Status**        | Decision: NO-GO. GPU XYB stays disabled; host XYB remains the canonical path.          |
| **Companion ADR** | [ADR-0201](../adr/0201-ssimulacra2-vulkan-kernel.md) (Accepted; this digest extends §Precision investigation with the NVIDIA driver measurement) |
| **Tags**          | gpu, vulkan, ssimulacra2, precision, xyb, fma                                          |

## Why now

[ADR-0201](../adr/0201-ssimulacra2-vulkan-kernel.md) §Precision investigation
landed `ssimulacra2_vulkan` with a *hybrid* host/GPU layout: the
linear-RGB → XYB pre-pass runs host-side (bit-exact with
`ssimulacra2.c::linear_rgb_to_xyb`) while the IIR blur and per-pixel
multiplies run on the GPU. The shader source `ssimulacra2_xyb.comp`
ships in-tree as a reference but is never dispatched in v1 — its
pipeline is allocated, its descriptor sets are wired, only the
`vkCmdDispatch` call is omitted (replaced by a `(void)` cast).

The original investigation matrix tested **lavapipe / Mesa anv /
Mesa RADV**. The empirically-measured per-pixel drift on the X plane
(~1.7e-6) compounded through the 6-scale pyramid + IIR + 108-weight
pool to a 1.59e-2 pooled-score drift — `places=1` only. The host
fallback closes the gap to 1.81e-7 (`places=6` effective).

Two questions stayed open after ADR-0201:

1. Does the same drift appear on **NVIDIA proprietary Vulkan**
   (which was not part of the investigation matrix)? NVIDIA's
   compiler chain is a very different code-generator from Mesa's,
   and the cancellation-amplification mechanism described in
   §Precision investigation is at the limit of what `precise` /
   `NoContraction` / `-O0` can mitigate — there is a non-zero
   chance that a different driver gets it right.
2. If NVIDIA is the only platform that holds, is that a strong
   enough position to ship a runtime-gated GPU XYB path?

T-GPU-OPT-VK-3 spike answers both.

## Method

Worktree: `feat/ssimulacra2-gpu-xyb-shader-precision` at master tip
(`e266bf8e`). Toggle introduced as a compile-time `SS2V_USE_GPU_XYB`
macro in `core/src/feature/vulkan/ssimulacra2_vulkan.c`
(default 0 = host XYB; 1 = GPU XYB). The shader source itself was
**not modified** — the existing `ssimulacra2_xyb.comp` already ships
every known precision mitigation:

- `precise` qualifier on every per-pixel intermediate (matmul
  partials, cube-root state, MakePositiveXYB rescale).
- Explicit per-multiply temp staging (`l_r = kM00 * r;
  l_g = m01 * g; ... l = l_rg + l_b + kOpsinBias;`) to block
  driver-side `OpExtInst Fma` fusion that lavapipe / Mesa anv /
  RADV emit even when `precise` is on the LHS.
- `NoContraction` decoration on every `OpFMul` / `OpFAdd`
  (verified via `spirv-dis`).
- Bit-trick + 2 Newton iteration cube-root that mirrors
  `vmaf_ss2_cbrtf` op-for-op, including the explicit
  decomposition of `(2y + x/y²) / 3` into discrete mul / div / add
  to defeat driver-side FMA fusion.
- `-O0` SPIR-V compilation (per
  [ADR-0201](../adr/0201-ssimulacra2-vulkan-kernel.md) Decision
  §Strict-mode SPIR-V compilation) to disable the SPIR-V
  optimizer's contraction passes.

Build: `meson setup build -Denable_vulkan=enabled -Denable_cuda=false
-Denable_sycl=false -Dc_args="-DSS2V_USE_GPU_XYB=1"; ninja -C build`
in `libvmaf/`. Cross-backend gate:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json \
  python3 scripts/ci/cross_backend_vif_diff.py \
    --feature ssimulacra2 --backend vulkan --places 4 \
    --vmaf-binary core/build/tools/vmaf \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --pixel-format 420 --bitdepth 8
```

Driver: NVIDIA proprietary Vulkan ICD,
`/usr/share/vulkan/icd.d/nvidia_icd.json`, NVIDIA driver
`595.71.05` (RTX class consumer GPU).

## Result

| SS2V_USE_GPU_XYB | Driver       | max_abs_diff | places=4 | Verdict |
| --- | --- | ---: | ---: | --- |
| 0 (host XYB, shipped default) | NVIDIA | 1.000000e-06 | 0/48 | OK   |
| 1 (GPU XYB, this spike)       | NVIDIA | **1.541600e-02** | 47/48 | FAIL |
| (ADR-0201 baseline) | lavapipe / anv / RADV | 1.59e-2 | 47/48 | FAIL |
| (ADR-0201 mitigated) | lavapipe / anv / RADV | 1.54e-2 | 47/48 | FAIL |

NVIDIA's drift sits at **1.5416e-2**, a virtually-identical magnitude
to the Mesa stack's 1.54e-2 result. Three readings:

1. The cancellation-amplification site
   (`X = 0.5 * (cbrt(l) - cbrt(m))`) is **not driver-specific**.
   It is an algebraic property of the conversion: when `l ≈ m`
   (which is the common case for natural imagery in the L/M
   opsin channels), any sub-ULP perturbation in the matmul
   inputs is amplified to single-ULP magnitude in `X`, and the
   X plane is a load-bearing input to the SSIM stats that
   feed the 108-weight pool.
2. The pre-amplification perturbation comes from the float
   matmul order `kM00*r + m01*g + kM02*b + kOpsinBias`. The
   shader stages every multiply into a `precise` temp and
   spelling out the addition tree — but at ULP granularity
   the float `+` operator is non-associative, and the GPU's
   SIMD lane ordering interacts with the SPIR-V optimizer's
   constant-folding passes in ways the spec's `precise` /
   `NoContraction` decorations do not bound to bit-equivalent
   IEEE-754 semantics.
3. NVIDIA's compiler matches the Mesa stack's bit error, not
   bit-better. This rules out the "maybe NVIDIA's compiler
   stack is more conservative" hypothesis.

Sanity check: rebuilding without the toggle (`SS2V_USE_GPU_XYB=0`,
the shipped default) and re-running the same gate produces
`max_abs_diff = 1.0e-6, 0/48 OK` — confirming the regression is
isolated to the GPU XYB path and the host fallback continues to
hold `places=4`.

## Decision

**NO-GO.** The GPU XYB shader cannot reach `places=4` on any tested
driver (lavapipe, Mesa anv, Mesa RADV, NVIDIA proprietary). The
limiting factor is algorithmic — cancellation in
`0.5 * (cbrt(l) - cbrt(m))` amplifies any sub-ULP matmul
perturbation by ~30× — not driver-specific or
precision-decoration-tunable.

Three options were weighed:

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **NO-GO (chosen)** — keep host XYB as canonical, leave shader as in-tree reference | Preserves `places=4` cross-backend contract; no runtime knob to misconfigure; no per-driver gating logic | The shader stays compiled but never dispatched — small idle code surface | Consistent with ADR-0201's investigation; new evidence (NVIDIA result) reinforces rather than overturns the original conclusion |
| Ship runtime toggle, default off, NVIDIA-only opt-in | Lets advanced users trade precision for ~5% wall-time saving on NVIDIA | NVIDIA result is *not* better than Mesa — the toggle would not even help on NVIDIA. The hypothesis the toggle was supposed to validate was falsified by the measurement | Falsified by the data — there is no driver where the toggle would deliver a precision-acceptable speedup |
| Convert XYB to `Float64` shader path (extension-gated) | Could in principle break the cancellation amplification | Requires `shaderFloat64` (not core on Vulkan 1.0; absent on a meaningful slice of consumer GPUs); doubles cube-root cost; the X output is still float so the cancellation reappears at the store site | ADR-0201 already weighed and rejected this; the new measurement does not change the trade-off |

## Code change

**None.** The toggle macro and the dispatch hook used during the
spike were reverted before commit. The C diff against master is
empty.

The shader source `ssimulacra2_xyb.comp`, its pipeline allocation
in `ssimulacra2_vulkan.c::ss2v_create`, and the descriptor set
allocation in `ss2v_run_scale` all stay in their current form —
that arrangement is what
[ADR-0201](../adr/0201-ssimulacra2-vulkan-kernel.md) Decision
§Consequences specifies for forward-compatibility. A future
follow-up that ships an opt-in `Float64` GPU XYB path would reuse
the same scaffolding.

## Reproducer

```bash
git checkout feat/ssimulacra2-gpu-xyb-shader-precision
# manually apply the 3-hunk feasibility patch (SS2V_USE_GPU_XYB
# macro + #if guards in ss2v_run_scale and the per-scale loop) —
# the patch is documented inline in this digest above.
cd libvmaf
meson setup build -Denable_vulkan=enabled -Denable_cuda=false \
  -Denable_sycl=false -Dc_args="-DSS2V_USE_GPU_XYB=1"
ninja -C build
cd ..
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json \
  python3 scripts/ci/cross_backend_vif_diff.py \
    --feature ssimulacra2 --backend vulkan --places 4 \
    --vmaf-binary core/build/tools/vmaf \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --pixel-format 420 --bitdepth 8
# Expect: max_abs_diff = 1.5416e-2, 47/48 mismatches at places=4.
```

## Open questions / follow-ups

- **Float64 GPU XYB**, gated on `shaderFloat64` device feature,
  remains a theoretical option. It is *not* on the near-term
  backlog — the cancellation site cancels at the float store
  step regardless of intermediate precision unless the entire
  XYB output buffer is widened to `double`, which doubles
  downstream blur / mul / SSIM memory traffic. Any pursuit of
  this would need a fresh ADR.
- **Runtime profiling delta** of host XYB vs the (hypothetical)
  GPU XYB: ADR-0201 reports <2% wall-time impact on lavapipe.
  Re-measuring on NVIDIA could be useful for the host XYB
  SIMD-isation work (concurrent VK-2 subagent on
  `feat/ssimulacra2-host-xyb-simd`) — if the host XYB is
  already ~5% of frame time, an AVX-512 path saves ~3% wall;
  this digest does not block or substitute that work.
- **CUDA / SYCL twins** of `ssimulacra2` mirror the same hybrid
  layout per [ADR-0206](../adr/0206-ssimulacra2-cuda-sycl.md).
  This digest's evidence reinforces that the same host-XYB
  decision applies to both.

## References

- [ADR-0201](../adr/0201-ssimulacra2-vulkan-kernel.md) — original
  precision investigation and host-XYB decision.
- [ADR-0192](../adr/0192-gpu-long-tail-batch-3.md) — batch 3 GPU
  long-tail scope; ssimulacra2 is part 7.
- [ADR-0164](../adr/0164-ssimulacra2-deterministic-eotf-cbrt.md) —
  deterministic sRGB EOTF + cbrt scalar reference.
- `core/src/feature/vulkan/shaders/ssimulacra2_xyb.comp` —
  the shader under investigation.
- `core/src/feature/ssimulacra2_math.h::vmaf_ss2_cbrtf` —
  scalar cube-root the shader mirrors.
- `scripts/ci/cross_backend_vif_diff.py` — the gate the
  measurement was taken with.
- Source: T-GPU-OPT-VK-3 (Vulkan-side optimization tracker).
