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
- HIP feature extractors: the three no-split citations in
  `ssimulacra2_hip.c` are withdrawn and the comments now match the code
  (ADR-1289). `ss2h_picture_to_linear_rgb()`, `ss2h_run_scale_gpu()` and
  `extract_fex_hip()` claimed an ADR-0141 §2 carve-out on the grounds
  that splitting them would break a line-for-line diff against the CPU
  source and the CUDA twin; HIP-vs-CPU agreement is proved by
  `test_hip_ssimulacra2_parity.c` and the ADR-0214 cross-backend gate
  instead, and praetor's touched-file rule admits no carve-out. The
  three `NOLINTNEXTLINE(readability-function-size)` suppressions are
  removed rather than relocated, and the invariants that do hold — the
  ADR-1205 / ADR-0891 `fmaf()` chain, the eight-launch order, the
  per-scale order — are stated where they apply. The CPU source and the
  CUDA twin are unchanged and keep their own citations.
