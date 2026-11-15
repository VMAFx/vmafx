---
paths:
  - core/src/pdjson.c
  - core/src/pdjson.h
  - core/src/framesync.c
invariant: pdjson enforces 512 container limit; pthread init, malloc, and size bounds checked without leak.
area: engine
---
<!-- markdownlint-disable MD013 -->
# Memory safety, pdjson bounds, and concurrency invariants

## Mandatory safety invariants

`pdjson.c` enforces ADR-1061 limit as **512 containers**, not zero-based
maximum stack index. `push()` checks next depth and completes allocation
before publishing `stack_top`; depth or allocation failure must leave
accepted stack intact. Reject zero or oversized `PDJSON_STACK_INC` overrides
before invoking allocator; retain separately compiled growth tests.
Preserve first-error diagnostic, streaming/peek/reset
contract, UTF-8 validation and private getter const qualifiers. Dedicated
`core/test/test_pdjson.c` suite covers these contracts. Never restore old
blanket NOLINT; only file-wide exception is ADR-1138's C `NULL` compatibility.
`pdjson.h` explicitly assigns sequential integer values to all enumerators in
`enum json_type` (`JSON_NONE = 0` .. `JSON_NULL = 11`), satisfying MISRA C++ /
AV Rule 145 (CodeQL `cpp/irregular-enum-init`, Alert 1064) while preserving
public ABI. Dedicated `core/test/test_pdjson.c::test_enum_json_type_abi_contract`
pins this contract. **Rebase-sensitive:** do not remove sequential assignments
or alter enumerator order on upstream sync or rebase.

Following invariants established during 2026-05-16 memory-safety
audit (findings #7, #8, #10). Every PR that touches affected files — or
adds new code in same category — must preserve them.

### 1. Every `pthread_*_init` return value must be checked (finding #7)

`pthread_mutex_init`, `pthread_cond_init`, and `pthread_rwlock_init` return
non-zero on `ENOMEM` on some POSIX implementations (embedded, musl-based
systems). Ignoring return value leaves pool or lock object in
undefined state; next `pthread_mutex_lock` call = undefined behaviour.

Pattern to follow: staged init with teardown of already-initialised
primitives on failure (see `vmaf_thread_pool_create` in
[`thread_pool.c`](../thread_pool.c)).

`vmaf_framesync_init` follows staged contract for both mutexes and its
condition variable. Sets caller output to `NULL` before allocation.
Publishes context only after every primitive and first queue node ready.
Returns negated pthread error; destroys only primitives whose initializers
succeeded. Do not move `*fs_ctx = ctx` back above those stages or collapse
ordered unwind labels.

### 2. Every `aligned_malloc` / `malloc` must NULL-check before use (#8)

Missing NULL check after `aligned_malloc` causes null-pointer dereference
on OOM (ASan-detected). In hot-path functions such as `adm_dwt2_*` in
[`feature/adm_tools.c`](../feature/adm_tools.c): allocation must be
NULL-checked with function returning error, or buffer must be
pre-allocated in extractor `init` callback so per-frame path stays
allocation-free. See Power of 10 rule 3 and CERT MEM30-C.

### 3. Size-computing functions must bound-check `w`/`h` first (#10)

When `w` is large enough that `(w + ALIGN - 1u)` wraps on `unsigned`
arithmetic, resulting aligned size = 0. Allocator succeeds, and any
pixel read is OOB. Add early-exit `if (w == 0 || w > 32768u || ...)
return -EINVAL;` guard at public entry point before any arithmetic.
Pattern: see `vmaf_picture_alloc` in [`picture.c`](../picture.c). CERT INT30-C.
