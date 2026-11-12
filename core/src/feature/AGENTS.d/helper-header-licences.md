---
paths:
  - core/src/feature/cuda/speed/speed_cuda_params.h
  - core/src/feature/hip/float_adm/float_adm_hip_math.h
  - core/src/feature/hip/float_ssim/ssim_decimate.h
  - core/src/feature/hip/integer_ciede/ciede_hip_math.h
  - core/src/feature/metal/float_ms_ssim_option_semantics.h
  - core/src/feature/sycl/sycl_ciede_math.h
  - core/src/feature/sycl/sycl_integer_ssim_math.h
  - core/src/feature/sycl/sycl_ssim_terms.h
  - core/src/feature/sycl/sycl_ssimulacra2_math.h
invariant: Helper header = EUPL-1.2 AND licences of exactly code reproduced, notices added; no reference code = EUPL-1.2.
area: simd-and-gpu
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Helper Header Licence Invariant

## Rebase-sensitive invariants

- **Helper header replaying reference arithmetic = `EUPL-1.2 AND` licences of
  exactly reproduced code
  ([ADR-1474](../../../../docs/adr/1474-relicense-helper-headers-and-ci-check.md)).**
  Upstream notices above `Copyright 2026 Lusoris`; fork notice stays. Family
  default too coarse: `gpu-ssim` names Netflix + IQA + Xiph.Org, helper holds
  one part. Origins per file in `scripts/dev/relicense_provenance.toml`
  `[ports]`: `ssim_decimate.h`, `sycl_ssim_terms.h` -> Netflix float SSIM +
  IQA; `sycl_integer_ssim_math.h` -> Xiph.Org only
  (`EUPL-1.2 AND BSD-2-Clause`); `float_ms_ssim_option_semantics.h` -> Netflix
  only; `sycl_ssimulacra2_math.h` -> libjxl (file name starts `sycl_`, family
  pattern needs `ssimulacra2` first).
- **Helper header holding no reference code = `EUPL-1.2`, `[not_ports]` entry
  with reason.** Argument block (`speed_cuda_params.h`), or macros + include of
  shared header carrying notices (`float_adm_hip_math.h` ->
  `float_adm_gpu_common.h`; `ciede_hip_math.h`, `sycl_ciede_math.h` ->
  `ciede_ff_math.h`). No upstream notice on such file: notice names holder of
  code present, nothing else.
- **New helper header in kernel directory**: run
  `python3 scripts/dev/relicense_fork_files.py --check`. `attribute <path>` ->
  read file against reference first. Reproduces family code -> `--write`.
  Reproduces part -> `[ports."<path>"] from = [...]`, then `--write`.
  Reproduces none -> `[not_ports]` line. Never hand-pick tag; never drop
  notice.
- Guard: `scripts/dev/tests/test_relicense_fork_files.py`
  (`test_a_helper_header_resolves_to_exactly_the_code_it_holds`,
  `test_a_header_that_only_configures_a_shared_header_is_not_a_port`), CI job
  runs `--check`.
