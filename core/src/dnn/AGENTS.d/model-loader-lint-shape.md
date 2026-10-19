---
paths:
  - core/src/dnn/model_loader.c
invariant: Model loader preserves helper decomposition, parenthesized tolower, and getenv suppression for tidy compliance.
---
<!-- markdownlint-disable MD013 -->
# Model Loader Static Analysis and Lint Shape

## Invariant — model_loader.c lint shape (ADR-1142 / ADR-0488)

`model_loader.c` measured by whole-tree clang-tidy ratchet
(`scripts/ci/tidy-baseline-cpu.json`), sits at zero. Three shapes in
file load-bearing for that; never collapse on rebase:

- **`parse_sidecar_*()` split.** `vmaf_dnn_sidecar_load()` once one
  202-line / 167-statement / 37-branch function. Now fixed-order driver
  over `sidecar_json_path()`, `slurp_sidecar_json()` and one
  `parse_sidecar_<field group>()` helper. Call order = parse order of
  original function; keeps "later field wins" behaviour of malformed
  sidecar identical. Merging helper back inline re-opens
  `readability-function-size`.
- **`str_to_lower()` calls `(tolower)` parenthesised.** glibc
  `<ctype.h>` defines `tolower` as five-level nested macro;
  expansion — not loop — tripped nesting-depth budget.
  Parenthesised form suppresses expansion, calls library
  function; behaviourally identical. Never "clean up"
  parentheses.
- **Environment reads cite ADR-0488 caller-contract**, same posture
  as `gpu_dispatch_env.cpp` and `core/src/mcp/compute_vmaf.c`.
  `vmaf_dnn_validate_onnx()` reads `VMAF_TINY_MODEL_DIR` through
  `vmaf_getenv_portable()` (`compat/crt_portable.h`; `getenv_s()` under
  MSVC and icx-cl, whose CRT deprecates `getenv()`). Value consumed
  before next environment read: under MSVC helper buffer per thread.
  `vmaf_dnn_verify_signature()` (`PATH`, POSIX-only) keeps one
  `NOLINTNEXTLINE(concurrency-mt-unsafe)` on `getenv()`. No
  `pthread_once` snapshot here: tiny-model tests `setenv()`
  `VMAF_TINY_MODEL_DIR` between cases, each case reads current value.
