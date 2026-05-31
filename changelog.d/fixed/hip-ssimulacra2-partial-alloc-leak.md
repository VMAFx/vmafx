- **HIP `ssimulacra2_hip` partial-allocation leak on init failure.**
  When `ss2h_alloc_device` or `ss2h_alloc_pinned` failed on any
  individual `hipMalloc` / `hipHostMalloc` past the first, the helpers
  returned early without freeing the prior successful allocations,
  and the init function's `fail_mod_mul` unwind label only unloaded
  modules + destroyed the stream — leaving up to 9 device buffers and
  14 pinned-host buffers leaked on every failed init cycle. Extracted
  null-guarded `ss2h_free_device_buffers` / `ss2h_free_pinned_buffers`
  helpers and called them from (1) each allocator's per-allocation
  failure path, (2) `fail_mod_mul` in init, and (3) `close_fex_hip`
  (DRY'd the previously inlined macros). Identified by hip-reviewer
  agent audit 2026-05-30 (HIGH severity).
