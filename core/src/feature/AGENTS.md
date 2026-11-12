<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 MD032 MD060 -->
# AGENTS.md — core/src/feature

Generated index. Pick area for touched paths (both if 2 fit); match `Touching`,
read each matching page first. Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../docs/development/agents-index.md)).

Orientation for agents working on feature extractors (VMAF metric
components: VIF, ADM, motion, integer-valued VIF/ADM/motion, CIEDE, CAMBI,
PSNR, SSIM, MS-SSIM, LPIPS, …). Parent: [../../AGENTS.md](../../AGENTS.md).

## Areas

| Area | Read first | Covers |
| --- | --- | --- |
| ADM extractors and twins | [adm](AGENTS-adm.md) | integer_adm*, float_adm*, adm_*, barten_csf_tools.h, core/src/rust/feature/adm/**, and ADM SIMD/GPU twins (x86/adm_*, arm64/*adm*, cuda/*adm*, sycl/*adm*, hip/*adm*, metal/*adm*). |
| Collector, registry, and shared rules | [framework](AGENTS-framework.md) | feature_extractor*, feature_collector*, feature_name.cpp, picture_copy*, perceptual_weight.c, pelorus_udu_wrap.h, float_*.c, integer_adm*, adm_*, barten_csf_tools.h, integer_vif.c, vif_tools.c, integer_ssim.c, brisque*, cambi.c, ciede.c, delta_e_itp.c, niqe.c, pu21.c, speed*.c, ssimulacra2.c, y_funque_plus.c, sycl/*adm*; M_PI/M_E, close_fex, alloc/fmaf bans, min-dim, unwind. |
| SIMD, GPU twins, and shared kernels | [simd-and-gpu](AGENTS-simd-and-gpu.md) | common/convolution*, iqa/*, simd_dx.h, compat_builtin.h, arm64/*sve2*, cuda/*, hip/*, sycl/*, metal/*, GPU option-table parity, row strides, helper-header licences, and ISA invariance. |
| SSIM, CAMBI, SpEED, and metrics | [ssim-and-metrics](AGENTS-ssim-and-metrics.md) | ssim*, ms_ssim*, float_ssim.c, float_ms_ssim.c, ssimulacra2*, cambi*, speed*, ciede.c, delta_e_itp.c, brisque*, niqe*, y_funque_plus.c, feature_lpips.c, feature_dists.c, feature_mobilesal.c, fastdvdnet_pre.c, and transnet_v2*. |
| VIF, motion, PSNR, and moment | [vif-motion-psnr](AGENTS-vif-motion-psnr.md) | vif*, integer_vif.c, float_vif.c, motion*, integer_motion*, float_motion.c, psnr*, float_psnr*, integer_psnr.c, third_party/xiph/psnr_hvs.c, moment*, and float_moment*. |
