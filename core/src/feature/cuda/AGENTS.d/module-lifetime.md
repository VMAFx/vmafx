---
paths:
  - core/src/feature/cuda/integer_adm_cuda.c
  - core/src/feature/cuda/integer_cambi_cuda.c
invariant: Every successfully loaded CUDA module has owned handle and context.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Rebase-sensitive invariants

- **Every successfully loaded CUDA module has owned handle and context-owned
  unload path ([ADR-1336](../../../../../docs/adr/1336-cuda-context-owned-resource-teardown.md)).**
  Any extractor that calls `cuModuleLoadData` must retain resulting
  `CUmodule` in its state and call `vmaf_cuda_module_unload` on every matching
  close or init-unwind path after pending device work is synchronized. Custom
  streams and events use `vmaf_cuda_stream_destroy` and
  `vmaf_cuda_event_destroy` for same reason: raw driver teardown acts on
  current context, which may be absent or belong to another caller at
  close. Guard partially initialized handles, clear them only after confirmed
  destruction, preserve first teardown error, and continue best-effort
  cleanup. Preserve four-module teardown in `integer_adm_cuda.c` and
  two-module teardown in `ssimulacra2_cuda.c`. complete inventory in
  `test_cuda_module_lifecycle_contract.py` must change with every new owner.
