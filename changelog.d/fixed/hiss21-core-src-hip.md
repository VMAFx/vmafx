- HIP feature extractors and backend runtime: the `goto`-based
  init/allocation cleanup ladders in `integer_adm_hip.c`,
  `integer_cambi_hip.c`, `integer_ms_ssim_hip.c`,
  `integer_psnr_hvs_hip.c`, `speed_chroma_hip.c`,
  `speed_temporal_hip.c` and `ssimulacra2_hip.c` were rewritten as
  cascading `static` unwind helpers (HISS-01, NASA Rule 1), and the
  oversized init / submit / collect / close / score-writer functions in
  the same files were split into cohesive `static` helpers (HISS-04,
  NASA Rule 4). Each former `fail_*:` label became one helper that
  releases exactly its own acquisition and then tail-calls the label it
  used to fall into, so the release **set** and release **order** are
  unchanged on every exit path — including the paths that deliberately
  skipped a tier or reported the `hipSuccess` left by the last
  successful call. No arithmetic expression was split across a helper
  boundary and no accumulation order was touched, so every score is
  bit-identical. Behaviour-preserving refactor only; no user-visible
  change.
- HIP feature extractors: the four `VmafOption` tables and the
  SSIMULACRA2 `g_weights[108]` vector are now hand-packed inside
  `// clang-format off` fences, the same treatment
  `core/src/feature/speed.c` gives the CPU SpEED tables, so each block
  stays inside the HISS-04 60-line bound. Every option name, alias, help
  string, default, range and flag — and the array order — is unchanged,
  so no CLI surface, feature-name key or model lookup moves.
- HIP backend runtime: the `g_hip_features[]` dispatch table in
  `core/src/hip/dispatch_strategy.c` is packed the same way, so it also
  stays inside the HISS-04 60-line bound. The feature-name strings,
  their per-extractor comment headers and the array order (with the
  `NULL` sentinel last) are unchanged.
