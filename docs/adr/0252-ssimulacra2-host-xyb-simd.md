<!-- markdownlint-disable MD013 MD060 -->
# ADR-0252: ssimulacra2 Vulkan host-path AVX2 + NEON SIMD (T-GPU-OPT-VK-2)

- **Status**: Accepted
- **Date**: 2026-05-02
- **Deciders**: Lusoris
- **Tags**: `simd`, `vulkan`, `ssimulacra2`, `performance`

## Context

The 2026-05-02 Vulkan hotpath profile (worktree
`profile-vulkan/docs/development/vulkan-dedup-profile-2026-05-02.md`) identified
three host-side functions in `ssimulacra2_vulkan.c` as the top CPU bottlenecks
for the `ssimulacra2_vulkan` extractor:

- **Top-1**: `ss2v_picture_to_linear_rgb` — scalar per-pixel YUV→linear-RGB conversion,
  559K pixel-ops/frame, ~9.2 s user / 10.3 s elapsed for 48 frames.
- **Top-2**: `ss2v_host_linear_rgb_to_xyb` — scalar per-pixel LMS matmul + cbrtf + bias,
  called once per scale × 6 scales × 2 pictures ≈ 3.4M scalar FMAs/frame.
- **Top-4**: `ss2v_downsample_2x2` + `malloc`/`free` × 5 — scalar 2×2 box average
  over 3 planes per inter-scale step.

The Vulkan extractor keeps these three steps on the CPU (rather than in GPU shaders)
because bit-exactness with the CPU ssimulacra2 extractor is required by the
precision contract (ADR-0192, places=2). The GPU XYB shader produced ~1.7e-6
per-pixel drift even with `precise` + `NoContraction` decorations
(ADR-0201 §Precision investigation), which compounded to places=1.

The scalar SIMD kernels in `ssimulacra2_avx2.c` / `ssimulacra2_neon.c` cannot be
reused directly because the Vulkan pyramid uses a fixed `plane_stride` equal to the
full-resolution frame size at every downsampled scale (to keep GPU shader channel
offsets constant at `c * full_plane`), whereas the CPU-extractor SIMD functions
assume `plane_stride = w * h`.

## Decision

Add new SIMD entry points with an explicit `plane_stride` parameter:

- `ssimulacra2_host_linear_rgb_to_xyb_avx2` / `_neon` in new TUs
  `feature/x86/ssimulacra2_host_avx2.c` and `feature/arm64/ssimulacra2_host_neon.c`.
- `ssimulacra2_host_downsample_2x2_avx2` / `_neon` in the same TUs.

Wire runtime dispatch into `ssimulacra2_vulkan.c`'s `init()` via
`vmaf_get_cpu_flags_x86()` / `vmaf_get_cpu_flags_arm()` and call via function
pointers in `Ssimu2VkState`. The existing `ssimulacra2_picture_to_linear_rgb_avx2`
(CPU-extractor signature) is reused for the YUV→linear-RGB hot path by building
a `simd_plane_t[3]` from the `VmafPicture` inside the Vulkan file (same pattern as
the CPU extractor's `convert_picture_to_linear_rgb`).

Bit-exactness contract: ADR-0161 — lane-commutative pointwise arithmetic,
per-lane scalar `vmaf_ss2_cbrtf`, `#pragma STDC FP_CONTRACT OFF`,
`-ffp-contract=off` compile flag, addition order preserved left-to-right to
match the scalar reference in `ss2v_host_linear_rgb_to_xyb`. Tests extended in
`test_ssimulacra2_simd.c` (`test_host_xyb`, `test_host_downsample`) to verify
`memcmp`-level byte-exactness against the scalar reference.

Measured wall-clock speedup on the 576×324 benchmark (micro-benchmark, x86-64):

- XYB host kernel (6 scales, cbrtf-bound): **~2× scalar**.
- Downsample kernel (5 inter-scale steps): **~3.2× scalar**.

AVX-512 is omitted: the XYB kernel is dominated by `cbrtf` (per-lane scalar in
both AVX2 and AVX-512 paths), so the marginal benefit of 16-wide vs 8-wide
matmul is below the 30% threshold set in the task specification. The decision can
be revisited if a SIMD-native cbrtf approximation is adopted in a future ADR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Vectorise cbrtf with a polynomial approximation | 8–16× XYB speedup | Would break ADR-0161 bit-exactness contract (different cbrtf from `vmaf_ss2_cbrtf`) | Bit-exactness is non-negotiable |
| Move XYB+downsample into the GPU via a new shader variant | 0 host CPU cost | Re-introduces the ~1.7e-6 per-pixel drift ADR-0201 measured; increases GPU shader complexity | ADR-0192 precision contract |
| AVX-512 variants | 16-wide matmul | cbrtf is per-lane scalar regardless; marginal gain is <15% | Below the 30% threshold per task spec |
| Reuse existing CPU-extractor SIMD by adjusting stride externally | Fewer files | Would require callers to compact/expand plane data on each call; copies 3-plane pyramid buffers per scale | More memcpy overhead than benefit |

## References

- `req` (T-GPU-OPT-VK-2): the user-requested task to add AVX2+NEON SIMD for the
  ssimulacra2 Vulkan host hot paths, citing the profile report.
- ADR-0161: ssimulacra2 SIMD bit-exactness policy (AVX2 precedent).
- ADR-0192: GPU long-tail batch 3 precision contract (places=2).
- ADR-0201: ssimulacra2 Vulkan kernel design and precision investigation.
- ADR-0138, ADR-0139: general SIMD bit-exactness policy for IQA and SSIM kernels.
