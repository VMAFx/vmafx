<!-- markdownlint-disable MD060 -->
# SYCL kernel coverage round 2 — gap audit (2026-05-30)

Companion research digest for [ADR-0884](../adr/0884-sycl-kernel-coverage-round2.md).

## Inventory

The SYCL backend ships 18 kernel TUs under `core/src/feature/sycl/`:

| TU | Extractor name | Round-1 (PR #351) | Round-2 (this PR) | Round-3 candidate |
|---|---|:---:|:---:|:---:|
| `integer_psnr_sycl.cpp` | `psnr_sycl` | yes | | |
| `integer_vif_sycl.cpp` | `integer_vif_sycl` | yes | | |
| `integer_adm_sycl.cpp` | `adm_sycl` | | yes | |
| `integer_ciede_sycl.cpp` | `ciede_sycl` | | yes | |
| `integer_ssim_sycl.cpp` (integer fex) | `integer_ssim_sycl` | | yes | |
| `integer_ms_ssim_sycl.cpp` | `float_ms_ssim_sycl` | | yes | |
| `integer_motion_v2_sycl.cpp` | `motion_v2_sycl` | | yes | |
| `integer_cambi_sycl.cpp` | `cambi_sycl` | (existing smoke + score sanity) | | |
| `integer_motion_sycl.cpp` | `motion_sycl` | (existing motion3 parity) | | |
| `integer_ssim_sycl.cpp` (float fex) | `float_ssim_sycl` | | | yes |
| `float_psnr_sycl.cpp` | `float_psnr_sycl` | | | yes |
| `float_vif_sycl.cpp` | `float_vif_sycl` | | | yes |
| `float_adm_sycl.cpp` | `float_adm_sycl` | | | yes |
| `float_motion_sycl.cpp` | `float_motion_sycl` | | | yes |
| `integer_moment_sycl.cpp` | `float_moment_sycl` | | | yes |
| `integer_psnr_hvs_sycl.cpp` | `psnr_hvs_sycl` | | | yes |
| `speed_chroma_sycl.cpp` | `speed_chroma_sycl` | | | yes |
| `speed_temporal_sycl.cpp` | `speed_temporal_sycl` | | | yes |
| `ssimulacra2_sycl.cpp` | `ssimulacra2_sycl` | | | yes |

## Selection rationale

The five round-2 picks were chosen by:

1. **Model-headline impact**: ADM is the dominant feature in the
   shipping VMAF default model + 4k + phone-screen. A regression in
   the SYCL ADM DLM / CSF chain shifts the headline VMAF score on
   every Intel-Arc CHUG re-extract.
2. **No upstream parity gate**: skip kernels that already had a
   parity test (`cambi_sycl` smoke + score sanity, `motion_sycl`
   indirect via motion3) and the two round-1 picks (`psnr_sycl`,
   `integer_vif_sycl`).
3. **Hardware-specific risk**: `ciede_sycl` is the only colour-diff
   path that runs at full f64 precision on Intel iGPU + dGPU. The
   Vulkan equivalent has a documented f32/f64 NVIDIA gap
   ([T-VK-CIEDE-F32-F64](../state.md), ADR-0273). The SYCL path is
   the production fallback on Intel-Arc and needs an independent
   pin at places=4.
4. **Numerical sensitivity**: `float_ms_ssim_sycl`'s 5-scale exponent
   stack is the most delicate kernel in the SYCL SSIM family — a
   single off-by-one in pyramid stride shifts scale-4 stats by ~1 %.
   A single-point parity check at places=4 catches it where
   cross-backend ULP gates don't.
5. **Distinct kernel topology**: `motion_v2_sycl` is a separate
   kernel from the classic motion stack — different reduction
   topology, separate score-name space (`motion2_v2`, `motion3_v2`).
   The motion3 parity test does not exercise it.

The remaining 10 kernels go to round 3. The 5 `float_*` variants
share most of the kernel topology with their integer twins so are
lower-priority. `speed_chroma_sycl` / `speed_temporal_sycl` /
`ssimulacra2_sycl` / `integer_psnr_hvs_sycl` / `float_moment_sycl`
are all candidates pending a follow-up audit.

## Pattern

All five tests follow the round-1 / ADR-0868 / PR #351 layout:

```c
static char *run_cpu_<kernel>(double *score) { ... vmaf_use_feature(vmaf, "<cpu-name>", NULL); ... }
static char *run_sycl_<kernel>(double *score) {
    *score = NAN;
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }
    ... vmaf_use_feature(vmaf, "<sycl-name>", NULL); ...
    vmaf_sycl_state_free(&sycl_state);
}
static char *test_<kernel>_cpu_sycl_parity(void) {
    ... if (isnan(sycl_score)) return NULL; ...
    mu_assert("<kernel> CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)",
              delta <= 1e-4);
}
```

Skip-on-no-device is essential — CI runners without an Intel GPU
(GitHub-hosted, AMD-only self-hosted lanes) must still pass these
suites. The lavapipe SYCL-CI leg (ADR-0860) routes around this on
its own.

## Fixture sizing

| Kernel | Fixture | Constraint |
|---|---|---|
| `adm_sycl` | 256x144 YUV420P 8-bpc | 4-scale dyadic pyramid → min 32x18 after scale-3 decimation; 256x144 is comfortable. |
| `ciede_sycl` | 256x144 YUV420P 8-bpc | No scale constraint; chroma planes filled with non-uniform pattern so ΔE != 0. |
| `integer_ssim_sycl` | 256x144 YUV420P 8-bpc | `compute_scale(256, 144, 0)` = round(144/256) = 1. SYCL scale=1 required; CPU integer_ssim is unconditional scale=1. |
| `float_ms_ssim_sycl` | **256x192** YUV420P 8-bpc | `MS_SSIM_GAUSSIAN_LEN << (MS_SSIM_SCALES - 1)` = 11 << 4 = **176** minimum dim. 256x144 fails with -EINVAL. |
| `motion_v2_sycl` | 256x144 YUV420P 8-bpc × 2 frames | Score is `motion2_v2` at frame index 1 (needs previous-frame reference). |

## Reproducer

```bash
# Build SYCL-enabled libvmaf in the dev container (per CLAUDE.md §15)
docker exec vmaf-dev-mcp meson setup /workspace/build-sycl-round2 \
    /workspace/core -Denable_sycl=true
docker exec vmaf-dev-mcp ninja -C /workspace/build-sycl-round2

# Run the five new parity tests
docker exec vmaf-dev-mcp meson test -C /workspace/build-sycl-round2 \
    --suite=gpu test_sycl_adm_parity test_sycl_ciede_parity \
    test_sycl_ssim_parity test_sycl_ms_ssim_parity \
    test_sycl_motion_v2_parity
```

On a host with no SYCL device visible, each test prints
`[skip: no SYCL device]` and exits 0.

## Related

- ADR-0214 — cross-backend places=4 gate.
- ADR-0219 — motion3 GPU contract + original SYCL parity pattern.
- ADR-0868 — round 1.
- PR #351 — round 1 implementation.
- PR #293 — SYCL init-failure cleanup (touches adm/vif/speed_chroma/speed_temporal
  source files; no test-file overlap with this PR).
