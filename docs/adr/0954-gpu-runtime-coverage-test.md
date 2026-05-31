<!-- markdownlint-disable MD060 -->
# ADR-0954: Host-only unit test for shared GPU dispatch runtime

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris (with Claude Code agent)
- **Tags**: `test`, `gpu`, `cuda`, `hip`, `sycl`, `runtime`

## Context

The fork ships three production GPU backends (CUDA, HIP, SYCL) and one
scaffold-stage backend (Metal). Each backend has a `dispatch_strategy`
translation unit that translates a per-feature characteristics
descriptor + the matching environment-variable override into a concrete
submission strategy (direct stream submission vs graph capture / graph
replay). All four call into the shared `gpu_dispatch_env.c` (thread-safe
once-snapshot of `VMAF_<BACKEND>_DISPATCH` env variables) and
`gpu_dispatch_parse.h` (header-only inline tokeniser shared by every
backend, ADR-0483).

A 2026-05-31 runtime-files audit identified that the shared helpers
(`gpu_dispatch_env`, `gpu_dispatch_parse`) had **zero direct unit
coverage** — they are only exercised at integration-time via the
per-feature smoke tests, which on CI without a GPU all skip the
dispatch path. The CUDA selector (`vmaf_cuda_select_strategy`) and the
HIP support stub (`vmaf_hip_dispatch_supports`) likewise had no direct
unit coverage; the SYCL selector's env-override path was covered only
via implicit observation. A future regression in the shared parser (for
example a whitespace-handling bug, or an off-by-one in the strategy
table) would slip past CI silently and surface as "every feature uses
the wrong submission strategy" — a global perf regression with no
correctness signal.

The dispatch logic is pure C with no GPU SDK runtime dependency: the
CUDA TU uses only `<pthread.h>` + `<stdlib.h>`, the HIP TU only includes
the opaque `VmafHipContext` forward declaration, and the shared helpers
need only `<stdatomic.h>` + `<pthread.h>`. This makes a host-only unit
test feasible on every CI runner and on every developer workstation
regardless of GPU availability.

## Decision

We will add `core/test/test_gpu_dispatch_runtime.c`, a host-only unit
test executable wired into the `fast` test suite that exercises:

1. `gpu_dispatch_parse.h` — null-input declines, valid token matching,
   multi-token + whitespace handling, unknown-strategy decline,
   malformed-input decline.
2. `gpu_dispatch_env.c` — null-key handling, first-call-wins snapshot
   semantics (later `setenv` is not observed), distinct-key
   independence.
3. `core/src/cuda/dispatch_strategy.c` — `VMAF_CUDA_DISPATCH_DIRECT`
   default, per-feature env override decoding for both DIRECT and
   GRAPH_CAPTURE, NULL feature name short-circuit.
4. `core/src/hip/dispatch_strategy.c` — stub returns 0 for every
   input (NULL ctx, NULL feature, named feature, unknown feature)
   per ADR-0212.

The test compiles each backend's dispatch_strategy TU directly into
the test executable; no GPU dependency, no opt-in build flag. It runs
on every CI matrix lane that builds tests.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add coverage via existing `test_cuda_*` / `test_hip_smoke` files | Co-located with other backend tests | Those tests are gated by `enable_cuda` / `enable_hip` build options; CPU-only CI lanes never run them. Defeats the goal of catching regressions on every build. | Defeats coverage goal. |
| Mock the env helper with a separate stub | Avoids env mutation in tests | Would test our mock, not the real shared singleton. The bugs we want to catch live in the real `pthread_once` + atomic-fence interplay. | Tests the mock, not the code. |
| Per-backend test files (3 separate executables) | One TU per backend keeps the table small | Triples the meson wiring; CUDA + HIP + the shared helpers all need the same fixtures (env mutation, function-pointer table). | Premature decomposition. |
| Skip CUDA + HIP, test only the shared parser/env | Smallest scope | Misses the regression class "selector dispatches to the wrong strategy", which is what users actually see. | Insufficient coverage. |

## Consequences

- **Positive**: A whitespace-handling regression in the shared
  parser, an off-by-one in the strategy-name table, a missing
  `pthread_once` barrier, or a CUDA selector default-flip would
  all be caught by `meson test --suite=fast` on every CPU-only
  build matrix lane (no GPU required). Eleven new assertions
  across five focused tests cover the previously-uncovered
  shared GPU runtime surface.
- **Negative**: The test mutates the process environment via
  `setenv` for snapshot-semantics coverage. We use namespaced
  `VMAFX_TEST_DISPATCH_RUNTIME_*` keys distinct from any real
  `VMAF_*_DISPATCH` variable so the singleton table never collides
  with production keys. Single-threaded test setup keeps the
  `concurrency-mt-unsafe` linter happy with `// NOLINT` comments.
- **Neutral / follow-ups**: Future SYCL coverage (when the
  `dispatch_strategy.cpp` heuristic surface grows beyond
  trivial threshold logic) can extend this same test file by
  conditionally compiling the SYCL TU under
  `if get_option('enable_sycl')`.

## References

- ADR-0181 — CUDA / SYCL dispatch-strategy contract.
- ADR-0212 — HIP backend scaffold + runtime PR (T7-10b).
- ADR-0461 — `gpu_dispatch_env` thread-safe once-snapshot helper.
- ADR-0483 — `gpu_dispatch_parse.h` shared inline tokeniser.
- ADR-0840 — atomic acquire/release fence pattern on the env-snapshot table.
- ADR-0108 — Deep-dive deliverables rule (PR checklist).
- Reproducer: `meson test -C build-cpu test_gpu_dispatch_runtime`.
- Source: `req` (user instruction to push test coverage on backend
  runtime files in `core/src/{cuda,sycl,hip}/`, excluding kernels).
