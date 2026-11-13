---
paths:
  - core/src/picture.c
  - core/src/picture_pool.c
  - core/src/picture_pool.cpp
  - core/src/gpu_picture_pool.cpp
invariant: Out-parameter init clears handles on failure; teardown owners replace partial cleanup ladders.
area: engine
---
<!-- markdownlint-disable MD013 -->
# Out-parameter initialization and teardown ladders

## Out-parameter init functions must clear the handle on every failure

Functions with shape `int X_init(X **out, ...)` that publish
allocation via caller's `*out` must guarantee `*out == NULL` on any
non-success return — including failure paths that take internal `goto`
and free object before returning. Trap is combined-assignment
idiom `X *const p = *out = malloc(...);` which publishes pointer to
caller *before* later `goto free_*` paths free it.

If caller stores handle in long-lived context (e.g.
`VmafContext.cuda.ring_buffer`), natural teardown (`vmaf_close()` →
`X_close(*out)`) will then UAF on freed object. Fix is mechanical:
set `*out = NULL` after every `free()` in failure-cleanup chain (and
explicitly on early-malloc-failure path even though assignment
already stored NULL there). Contract this pins: "caller may inspect
`*out` only on success; non-zero return guarantees `*out == NULL`."

Pattern: see `vmaf_gpu_picture_pool_init` in
[`gpu_picture_pool.cpp`](../gpu_picture_pool.cpp). Regression test:
`core/test/test_gpu_picture_pool_uaf.c`. Since HISS-21 burn-down that
file carries no `goto`; clearing now happens in `gpu_pool_destruct`.

## Teardown owners replace the cleanup ladders (HISS-21)

`picture.c`, `picture_pool.c`, `picture_pool.cpp`, `gpu_picture_pool.cpp`,
`predict.c`, `read_json_model.c`, `mcp/mcp.c` and `interop/` hold no `goto`.
Each former label chain is now one named `static` teardown owner, or guard
clause that unwinds inline:

- `pool_destruct_partial(p, stage)` — `picture_pool.c` and `picture_pool.cpp`.
  `stage` counts completed acquisitions; guards run newest-first, so stage N
  frees what old label N freed, in old order. New resource: append one stage
  at end of enum plus one guard at **top** of helper.
- `gpu_pool_destruct` — `gpu_picture_pool.cpp`.
- `pool_return_index` / `pool_pop_slot` / `pool_attach_priv` — pool fetch.
  Only `pool_return_index` touches free list, so push-back and
  `pthread_cond_signal` of ADR-0960 stay in one place.
- `mcp_uds_listen` / `mcp_uds_publish` — `mcp/mcp.c`. Both leave
  `atomic_store(&server->uds_running, 0)` to caller, so that store stays
  last on every failure.

Rebase rule: conflict must not reintroduce early `return` between
acquisition and its owner, and must not reorder guards. Free order is
contract; `core/test/test_picture_pool_error_paths.c`,
`test_picture_pool_cpp_error_paths.c` and `test_gpu_picture_pool_partial_init.c`
pin it.

`vmaf_gpu_picture_pool_init` returns `-ENOMEM` on `malloc` failure with `*pool == nullptr`.
Never return success (`0`) or leave `*pool` un-cleared on pool struct or picture array
allocation failure. Pinned by `core/test/test_gpu_picture_pool_alloc_failure.c` (#1455).
