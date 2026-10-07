---
paths:
  - core/src/feature/cuda/integer_adm_cuda.c
  - core/src/feature/cuda/integer_ssim_cuda.c
invariant: Parent rules, license headers, include order, and no FMA contraction across all CUDA kernels.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Ground rules

- **Parent rules** apply (see [../AGENTS.md](../../AGENTS.md) +
  [../../AGENTS.md](../../../AGENTS.md) +
  [`../../cuda/AGENTS.md`](../../../cuda/AGENTS.md)).
- **Wholly-new fork files use dual Netflix + Lusoris/Claude
  copyright header** per [ADR-0025](../../../../../docs/adr/0025-copyright-handling-dual-notice.md).
  Many TUs here predate dual-notice rule, carry only
  Netflix header (with NVIDIA contributor lines on `integer_adm/`
  CUDA kernels) — correct for upstream-mirrored files; do
  not retro-fit.
- **`#include` order** mirrors SYCL / Vulkan twins:
  `feature_collector.h` / `feature_extractor.h` first, then
  `cuda/integer_<feature>_cuda.h`, then `cuda_helper.cuh` /
  `kernel_template.h`. Don't shuffle.
- **FMA contraction OFF for every kernel (ADR-1403).** Every fatbin takes
  `cuda_device_strict_fp_args` (`core/src/meson.build`, policy markers):
  nvcc `-Xcompiler=<host strict FP>` + `--fmad=false`; clang CUDA
  (`-Denable_nvcc=false`) `-ffp-contract=off`. Same model as CPU and SYCL
  (ADR-1367). `cuda_cu_extra_flags` = other private flags only, never FP
  flag; `test_strict_fp_compiler_args.py` rejects per-kernel copies,
  opt-outs, fatbin command without list, second definition.
  Reference fuses on purpose (`vmaf_fmaf_exact()` / `fmadd`:
  `ms_ssim_decimate.c`, `ssimulacra2.c`, SpEED) -> kernel spells
  `__fmaf_rn()`; never rely on compiler to fuse. New kernel: write
  reference's operations in reference's types; plain `a * b + c` rounds
  twice, as on host. Division and sqrt are
  IEEE (`-prec-div` / `-prec-sqrt` default true, no `--use_fast_math`).
  On rebase: keep flag.
