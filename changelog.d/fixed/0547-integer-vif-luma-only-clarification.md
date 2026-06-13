- `integer_vif`: clarified as luma-only by design across every backend
  (CPU AVX2/AVX-512, ARM NEON, CUDA, HIP, SYCL, Vulkan, Metal),
  matching upstream Netflix/vmaf and the Sheikh & Bovik (2006)
  definition of the metric (ADR-0547). The CUDA twin's misleading
  `n_planes = 1; /* …not yet implemented */` comment is rewritten to
  state the luma-only design intent. The vestigial `enable_chroma`
  option on the CUDA twin (introduced by an abandoned 2026-05-16
  multi-plane VIF attempt that never reached `master`) is retained
  for backward compatibility with callers that pass it on the CLI or
  in model JSONs, but now emits a one-shot warning when set to `true`
  instead of silently producing luma-only output that contradicts the
  request. `docs/metrics/vif.md` is corrected — it previously
  advertised `integer_vif_cb` / `integer_vif_cr` features and an
  `enable_chroma=true` invocation example that produced nothing on
  master. A new regression test
  (`core/test/test_integer_vif_cpu_cuda_parity.c`, suite
  `fast`/`gpu`) asserts CPU vs CUDA `vif_scaleN_score` parity within
  1e-4 and `enable_chroma=true` bit-identity with the default
  invocation. Closes 2026-05-18 deep-audit finding 23 as
  Confirmed-not-affected in `docs/state.md`.
