- **docs**: planning ADR-0345 + Research-0091 lock the cambi GPU port
  strategy for the three pending backends (CUDA, SYCL, HIP). All three
  inherit Strategy II host-staged hybrid from ADR-0205 / ADR-0210
  (Vulkan precedent shipped in PR #196 / T7-36) — GPU services the
  embarrassingly-parallel pre-passes; host residual runs unmodified
  `calculate_c_values` + spatial pooling on byte-identical buffers;
  cross-backend gate at `places=4` from day one. LOC envelopes per
  Research-0091: CUDA ≈1100 (LOW risk), SYCL ≈1300 (MEDIUM, dual
  toolchain matrix per ADR-0335), HIP ≈1100 (MEDIUM-LOW, hipify-perl
  seeded from CUDA). Implementation order: CUDA → SYCL → HIP. Per-port
  PRs follow per the digest's §6 ordered plan. No code in this PR.
