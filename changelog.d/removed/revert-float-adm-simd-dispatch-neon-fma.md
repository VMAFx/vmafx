### Removed

- Reverted float-ADM SIMD dispatch wiring (PR #685, commit `b1a6c0d62`): the
  `AdmSimdDispatch` table and `adm_prime_simd_dispatch()` call are removed from
  `adm_tools.c`/`.h`/`float_adm.c`. The NEON DWT2 kernel produced a 1-ULP FMA
  divergence on ARM CI that could not be suppressed reliably via
  `#pragma clang fp contract(off)`. The float-ADM SIMD kernels remain compiled
  but are no longer dispatched, restoring pre-PR-#685 behaviour. A follow-up PR
  will rewire the dispatch with an FMA-safe NEON DWT2 path. (ADR-1057)
