- **Const-correctness audit (`docs/research/const-correctness-audit-2026-05-30.md`):**
  swept 28 buildable fork-added C / C++ translation units (DNN, MCP,
  tiny-AI feature extractors, x86 SIMD, fork-added tools, gpu_dispatch_env)
  for pointer-parameter const-correctness gaps under MISRA C 8.13 / CERT
  EXP05-C / SEI CERT C++ Con01. **Zero actionable findings.** The
  pre-existing `.clang-tidy` `readability-non-const-parameter` gate plus
  the code-review patterns established in ADRs 0374 / 0461 / 0485 / 0550
  kept the surface clean unaided. Documentation-only PR; no code changes.
  HIP / Metal / SYCL backends and ARM64 NEON paths are deferred to a
  container-side re-run (see digest §Follow-ups).
