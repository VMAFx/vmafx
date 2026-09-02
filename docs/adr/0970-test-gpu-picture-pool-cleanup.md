<!-- markdownlint-disable MD013 MD060 -->
# ADR-0970: test_gpu_picture_pool.c: remove unused malloc + dead code (Round 27 audit D.3 + D.4)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: `testing`, `cuda`, `memory`, `cleanup`, `fork-local`

## Context

The Round 27 audit identified two bugs in `core/test/test_gpu_picture_pool.c`:

**D.3 — Unused malloc immediately overwritten (line 47):** The `VmafCudaCookie`
initialiser set `.state = malloc(sizeof(VmafCudaState))`, but the next statement
called `vmaf_cuda_state_init(&my_cookie.state, cu_cfg)`. The function signature
is `int vmaf_cuda_state_init(VmafCudaState **cu_state, VmafCudaConfiguration)` —
it allocates internally and writes the new pointer through `**cu_state`. The
`malloc` result was therefore unconditionally overwritten before it was ever read,
producing an unconditional memory leak that ASan reports on every CUDA-capable
run. The subsequent `free(my_cookie.state)` on the skip path freed the
internally-allocated pointer, not the leaked one, leaving the pre-allocated block
permanently unreachable.

**D.4 — Dead `/* ... */` block containing uncompilable code (lines 119–210):**
A `test_ring_buffer_threaded` function was wrapped in a C block comment. The
block contained two latent bugs:

1. A duplicate `cfg` declaration (`VmafCudaConfiguration cfg` followed by
   `VmafGpuPicturePoolConfig cfg` in the same scope) — this would fail to
   compile if uncommented.
2. A type-mismatched call `vmaf_cuda_state_init(my_cookie.state, cfg)` passing
   `VmafCudaState *` instead of the required `VmafCudaState **` — undefined
   behaviour if uncommented.

`git log --follow` shows that the dead block was copied verbatim from an
earlier `test_ring_buffer.c` when the file was first created as part of the
ADR-0239 pool-promotion refactor (PR #266, commit `19d7eda20`). The block
was already broken at that time — it was never intended to become an active
test. No subsequent commit attempted to fix or activate it. The PR #266 commit
message explicitly notes only `test_ring_buffer` (singular) under its test
plan; the threaded variant appears to be an abandoned prototype left over from
the pre-refactor implementation.

## Decision

Remove the unused `malloc(sizeof(VmafCudaState))` from the `VmafCudaCookie`
initialiser in `test_ring_buffer` (D.3 fix), and delete the entire dead block
including `test_ring_buffer_threaded`, its helper `request_picture`, and the
`MyThreadPoolData` typedef (D.4 fix). The commented-out `mu_run_test` call is
removed along with the block.

The dead block is not revived because: (a) it has two compilation errors that
would need fixing before it could run, (b) the threaded pool path is already
exercised by the production call sites that the CI matrix covers, and (c) adding
a threaded pool test is a new feature request, not a bug fix — it can be done as
a properly-designed test in a separate PR if coverage is later deemed necessary.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix bugs in dead block and uncomment | Adds threaded-pool test coverage | Two compilation errors to fix; `usleep`-based timing is fragile under sanitizer slow-down; requires `vmaf_picture_alloc` / `vmaf_picture_unref` linkage not currently in the test binary's deps | Dead block was an abandoned prototype, not a planned test; reviving it is out of scope for a bug fix |
| Leave dead block in place with a TODO comment | No diff | Dead code with compilation errors persists; future agents may misread it as intentional | Contradicts the AGENTS.md invariant being established by this ADR |

## Consequences

- **Positive**: The unconditional memory leak (D.3) is eliminated. The file no
  longer contains code that cannot compile (D.4). `core/test/AGENTS.md` gains
  an invariant preventing future dead-block accumulation.
- **Negative**: None. The threaded pool path remains untested at the unit-test
  level, but this was already the state before the fix.
- **Neutral / follow-ups**: If a threaded GPU picture pool test is desired, it
  should be written from scratch as `test_gpu_picture_pool_threaded.c` with
  correct CUDA-state init, no duplicate declarations, and proper sanitizer-safe
  timing (no `usleep`-based busy-waiting).

## References

- Round 27 audit, bugs D.3 and D.4.
- ADR-0239: pool-promotion refactor that introduced the file (PR #266, commit `19d7eda20`).
- `vmaf_cuda_state_init` signature: `core/include/libvmaf/libvmaf_cuda.h:46`.
- Related PR: this PR (fix/test-gpu-picture-pool-cleanup).
