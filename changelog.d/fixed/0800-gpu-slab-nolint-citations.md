- **GPU slab pointer-cast hygiene (ADR-0800)**: introduced `core/src/feature/gpu_slab.h`
  with the `SLAB_FIELD(dst, type, slab)` macro to centralise the `CUdeviceptr`/`uintptr_t`
  → typed-pointer cast that all GPU feature-extractor buffer-carving code shares.
  Replaced 21 bare `performance-no-int-to-ptr` NOLINTs in `integer_vif_hip.c`,
  `integer_vif_cuda.c`, and `integer_adm_cuda.c` with `SLAB_FIELD` calls — satisfying
  the ADR-0278 cited-NOLINT rule.  No math change; ADR-0214 GPU-parity gate unaffected.
