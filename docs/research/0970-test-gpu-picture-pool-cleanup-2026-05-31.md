# Research digest: test_gpu_picture_pool.c Round 27 cleanup (D.3 + D.4)

**Date**: 2026-05-31
**ADR**: [ADR-0970](../adr/0970-test-gpu-picture-pool-cleanup.md)
**PR**: fix/test-gpu-picture-pool-cleanup

## Findings

### D.3 — Unused malloc in VmafCudaCookie initialiser

`vmaf_cuda_state_init` has the signature:

```c
int vmaf_cuda_state_init(VmafCudaState **cu_state, VmafCudaConfiguration cfg);
```

The function allocates a `VmafCudaState` internally and writes the pointer
through the double-pointer output parameter. Any value already stored in
`*cu_state` before the call is unconditionally discarded.

The test initialised the cookie with `.state = malloc(sizeof(VmafCudaState))`,
then immediately called `vmaf_cuda_state_init(&my_cookie.state, cu_cfg)`. On
every code path — success, device-not-found, or any other error — the
pre-allocated block became unreachable the moment `vmaf_cuda_state_init`
wrote through `&my_cookie.state`. ASan would report this as a definite leak
on any CUDA-capable machine running the test.

The `free(my_cookie.state)` on the skip path (when `err || !my_cookie.state`)
freed the internally-allocated pointer (which may be NULL on failure), not the
leaked pre-allocated one. This was already the state before the fix — the
`free` on the skip path is correct after the fix (it guards against a partial
internal allocation on failure).

Fix: remove `.state = malloc(sizeof(VmafCudaState))` from the initialiser.
`my_cookie.state` is a pointer field; without an explicit initialiser it will
be zero-initialised by the `{0}`-style aggregate init (the struct literal does
not use `{0}` but designated initialisers leave unmentioned fields
zero-initialised per C11 §6.7.9 ¶21). `vmaf_cuda_state_init` overwrites it
unconditionally.

### D.4 — Dead `/* ... */` block: provenance and decision

`git log --all --follow` traces the file to commit `19d7eda20` (PR #266,
ADR-0239, 2026-05-02). The diff of that commit shows the dead block was
present from day one — it was copied verbatim from an earlier implementation
file during the ring-buffer → gpu-picture-pool rename, but the two compilation
errors (duplicate `cfg` declaration, missing `&` in `vmaf_cuda_state_init`
call) were also present in the source. No subsequent commit attempted to
uncomment or fix the block.

The PR #266 commit message test plan mentions only `test_ring_buffer`
(singular). `mu_run_test(test_ring_buffer_threaded)` was already commented out
in the same commit. There is no evidence of intent to activate the block.

The block is entirely dead: the preprocessor never sees it, the compiler never
sees it, and no test runner ever executes it. Keeping it creates ongoing
maintenance burden: any agent reading the file must reason about whether the
block is intentional and whether the latent bugs in it are real. Deleting it
removes ~90 lines of noise.

The threaded pool path is already exercised through the production call sites
that the CI matrix covers (CUDA backend integration tests). A dedicated
threaded-pool unit test would be a new feature, not a restoration of something
that was previously working.

## Conclusion

Both fixes are purely mechanical. No API, no behaviour, no test coverage
semantics change. The only observable effect is that D.3 eliminates an ASan
leak report on CUDA-capable machines.
