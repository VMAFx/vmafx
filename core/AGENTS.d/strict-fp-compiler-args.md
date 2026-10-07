---
paths:
  - core/src/meson.build
  - core/test/test_strict_fp_compiler_args.py
invariant: Strict FP is project argument; every C and C++ translation unit builds without contraction.
---
<!-- markdownlint-disable MD013 MD060 -->
# Strict FP compiler-argument policy (ADR-1461)

## Rebase-sensitive invariants

- **Strict FP = project argument (ADR-1461)**: `core/src/meson.build` holds
  `BEGIN/END VMAF strict FP compiler-argument policy` block above first build
  target, followed by `add_project_arguments(vmaf_strict_fp_args, language :
  ['c', 'cpp'])`. Every C / C++ TU (libraries, tools, tests) built without
  contraction; Metal `.mm` via `metal_objcpp_args`. On rebase: keep block +
  argument above first target (Meson refuses it later); never append
  `vmaf_fp_model_args` or contraction-enabling flag to target (`icx`:
  `-fp-model=precise` after `-ffp-contract=off` re-enables contraction);
  wanted FMA = intrinsic or `fma()` in source. Guard:
  `core/test/test_strict_fp_compiler_args.py` (reads build's own
  `compile_commands.json` under `meson test`). aarch64 check:
  `make test-netflix-golden-arm64`.
- **icx spells model reset (ADR-2170)**: `intel-llvm` strict line is
  `-fp-model=precise -fno-fast-math -fcomplex-arithmetic=full
  -ffp-contract=off` (SYCL: same, then fp32 precision pair, except for
  MSVC-syntax driver, which keeps `-fp-model=precise -ffp-contract=off`).
  two-flag spelling compiles to same objects but icx reports
  `-ffp-contract=off` after `-fp-model=precise` as `-Woverriding-option` on every
  compile, which gated leg cannot carry. On rebase keep `-fno-fast-math
  -fcomplex-arithmetic=full` between model and contraction flag;
  contraction flag stays last. Guard: `test_icx_strict_policy_draws_no_overriding_option_warning`
  (compiles with `-Werror`; old spelling must fail) and matrix in
  `core/test/test_strict_fp_compiler_args.py`.
