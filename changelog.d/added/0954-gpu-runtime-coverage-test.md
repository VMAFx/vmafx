## Added

- **`core/test/test_gpu_dispatch_runtime.c`** — host-only unit test
  (CPU-only, no GPU SDK required, runs on every CI matrix lane) that
  pins the previously-uncovered shared GPU dispatch runtime: the
  `gpu_dispatch_env.c` thread-safe once-snapshot helper (ADR-0461),
  the `gpu_dispatch_parse.h` shared inline tokeniser (ADR-0483), the
  `core/src/cuda/dispatch_strategy.c` selector (ADR-0181), and the
  `core/src/hip/dispatch_strategy.c` support stub (ADR-0212). 11
  assertions across 5 focused tests cover null-input handling,
  multi-token + whitespace parsing, snapshot first-call-wins
  semantics, per-feature env override decoding for both DIRECT and
  GRAPH_CAPTURE strategies, and stub-shape contracts. Wired into the
  `fast` suite as `meson test test_gpu_dispatch_runtime`. See ADR-0954.
