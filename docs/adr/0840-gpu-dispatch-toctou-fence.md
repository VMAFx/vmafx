<!-- markdownlint-disable MD013 MD060 -->
# ADR-0840: Fix cu_state leak on import failure and gpu_dispatch_env TOCTOU

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cuda`, `security`, `framework`, `ci`

## Context

Two independent correctness bugs were identified in the GPU dispatch path during an
audit (source: audit a4003b2235845d570):

**Bug 1 — cu_state leak (CWE-401, memory leak on error path):**
In `core/tools/vmaf.c`, `vmaf_cuda_state_init` allocates `cu_state`. If the
subsequent `vmaf_cuda_import_state` call fails, the function returned -1 without
calling `vmaf_cuda_state_free(cu_state)`, leaking the allocation for the lifetime of
the process. The leak only occurs on the error path (GPU init succeeds but
import fails), but it is a CWE-401 resource leak.

**Bug 2 — gpu_dispatch_env lock-free fast-path TOCTOU (LOW severity):**
`vmaf_gpu_dispatch_env_get` in `core/src/gpu_dispatch_env.c` uses a lock-free fast
path that reads `g_rows[i].var_name` and `g_rows[i].value` without memory fences.
On weakly-ordered architectures (ARM64, POWER), a CPU or compiler reorder can cause
the reader to observe a non-NULL `var_name` while `value` is still uninitialized.
The publisher inside the mutex wrote `value` first, then `var_name`, but without a
release fence the ordering is not guaranteed to be visible to observers outside the
lock. A paired release/acquire fence pair makes the ordering formally correct per
ISO C11 §7.17 and eliminates the TOCTOU hazard.

## Decision

Apply both fixes in the same PR:

1. Add `vmaf_cuda_state_free(cu_state);` before the `return -1` in the
   `vmaf_cuda_import_state` failure arm in `core/tools/vmaf.c`.

2. In `core/src/gpu_dispatch_env.c`:
   - Add `#include <stdatomic.h>`.
   - Insert `atomic_thread_fence(memory_order_release)` after writing `row->value`
     and before writing `row->var_name` on the publish path (inside the lock).
   - Insert `atomic_thread_fence(memory_order_acquire)` after matching
     `g_rows[i].var_name` and before reading `g_rows[i].value` on both fast-path
     loops.

Both are pure bug fixes with no interface change; no ADR would normally be required
(per CLAUDE.md §12 r8), but the weak-memory fence pattern is a non-obvious
correctness idiom that a future maintainer could accidentally revert — warranting
a decision record to explain the pairing invariant.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Full mutex on every fast-path read | Trivially correct | Defeats the purpose of the fast-path cache; adds lock contention on every GPU dispatch decision | Performance regression on the hot path |
| Switch to `_Atomic const char *` fields | Eliminates manual fences; idiomatic C11 | Requires changing the struct layout and all write sites; higher diff surface | Disproportionate churn for a two-line fix |
| Keep existing code, add TOCTOU comment | No code change | Leaves a real data race on ARM64 | Does not fix the bug |

## Consequences

- **Positive**: CWE-401 resource leak closed; lock-free read path is now formally
  correct on all architectures including ARM64 and POWER.
- **Negative**: Minimal — two `atomic_thread_fence` calls on an already-cold path
  (first call per var_name only); immeasurable overhead.
- **Neutral / follow-ups**: The fence pairing must be preserved if the publish
  path is ever refactored. Comment blocks in the code reference this ADR as the
  rationale.

## References

- Source audit reference: a4003b2235845d570
- CWE-401: Missing Release of Memory after Effective Lifetime
- ISO C11 §7.17 (atomics / memory model)
- ADR-0461: `gpu_dispatch_env` once-snapshotted pattern
- ADR-0157: `vmaf_cuda_state_free` API introduction (CUDA preallocation leak fix)
- Related PR: fix/gpu-dispatch-toctou-fence-20260529
