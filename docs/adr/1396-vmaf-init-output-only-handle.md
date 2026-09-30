<!-- markdownlint-disable MD013 MD060 -->
# ADR-1396: `vmaf_init()` treats its handle as output-only again

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: api, correctness, compatibility, memory-safety, fork-local

## Context

[ADR-1032](1032-vmaf-init-double-init-guard-vmaf-close-pointer-contract.md) Fix 1 made `vmaf_init()` return `-EINVAL` when `*vmaf` is not NULL, to stop a second `vmaf_init()` on an open handle from leaking the first context. The check reads the caller's incoming handle, and upstream Netflix/vmaf never does: its `vmaf_init()` writes `*vmaf` first, and its own callers leave the variable uninitialised. Examples are `libvmaf/tools/vmaf.c` (`VmafContext *vmaf; err = vmaf_init(&vmaf, cfg);`), `test/test_context.c` and `test/test_cuda_pic_preallocation.c`. A caller following that pattern gets `-EINVAL` whenever the stack slot happens to hold a non-zero value.

Measured on 2026-09-30 against fork master `10f27efe2`:

- Upstream's unmodified `test_context.c` failed `test_context_init_and_close` ("problem during vmaf_init") in 3 of 3 runs.
- Upstream's `test_cuda_pic_preallocation.c` got `-22` from `vmaf_init()` with the handle holding `0x736f70736e617254`. It then crashed in `vmaf_use_features_from_model()`, because that test checks only the pointer, not the return code.

[docs/api/index.md](../api/index.md) promises that the whole `libvmaf.h` surface comes from upstream and that the fork "preserves them verbatim", and it reserves source-compatibility breaks for a major version. The guard breaks that promise for code written against upstream. The public header never documented the precondition either.

The guard also cannot do its job reliably. An uninitialised handle and a handle that still holds an open context both arrive as a non-NULL value. The library cannot tell them apart without reading an indeterminate value, which is what goes wrong here.

## Decision

`vmaf_init()` never reads `*vmaf`. It sets `*vmaf = NULL` on entry and `*vmaf = v` once the context exists. The only argument check left is `vmaf == NULL` (`-EINVAL`). As a result:

- any `VmafContext *`, initialised or not, is accepted, as in upstream;
- the handle is NULL after every failure. This keeps the part of ADR-1032 that upstream lacks: upstream leaves a freed pointer in `*vmaf` when set-up fails (CERT MEM30-C).

A handle that still holds an open context is overwritten; the header says to close it first. This supersedes ADR-1032 Fix 1 only. Fix 2 (the `vmaf_close()` pointer contract) and Fix 3 (the DNN fp32 fallback) stand.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep the guard and document that `*vmaf` must be NULL on entry | Keeps `-EINVAL` for a real double initialisation | Callers written against upstream (its CLI, its tests) keep failing at random; it contradicts the stable-API promise in `docs/api/index.md`; it still reads an indeterminate value | Randomly failing upstream-compatible callers is worse than the leak it prevents |
| Keep a registry of live contexts and reject only a value that names one | Detects a real double initialisation of an open handle | A stale stack slot can hold the address of another live context (a program with several contexts), so false rejections remain; adds global state and a lock to every init and close; still reads the indeterminate value | Complexity without removing the failure mode |
| **Output-only handle, NULL on entry (chosen)** | Matches upstream's contract; deterministic; keeps the no-dangling-handle property after a failure | A second `vmaf_init()` on an open handle leaks the first context without an error, as in upstream | Chosen: the leak is a caller bug that upstream also leaves to the caller, and the header now says so |

## Consequences

- **Positive**: code written against upstream libvmaf runs on the fork again. Upstream's `test_context.c` passes 2/2 against the fixed library (3 runs, ASan+UBSan, leak detection on), and so does its `test_cuda_pic_preallocation.c` (5/5 on an RTX 4090). `*vmaf` is well defined after every call.
- **Negative**: a caller that calls `vmaf_init()` twice on an open handle no longer gets `-EINVAL`; the first context leaks unless the caller closes it. No in-tree caller does this. The bindings (Rust `vmafx`/`vmafx-sys`, Go `pkg/libvmaf`), the FFmpeg patches and every in-tree C caller start from a NULL handle or a zeroed struct.
- **Neutral / follow-ups**: `core/test/test_context.c` replaces `test_vmaf_init_double_init_guard` with `test_vmaf_init_ignores_the_incoming_handle` (a garbage-filled handle must succeed; it fails on the guard) and `test_vmaf_init_overwrites_an_open_handle`. The `vmaf_init()` documentation in `libvmaf.h` and `docs/api/index.md` states the contract. ADR-1032's status line records the partial supersession; its body is unchanged.

## References

- req: task brief (2026-09-30): "Bugs you find on the way are to be FIXED, not just recorded, even when they predate your change: fix them in your PR if in scope, otherwise in a separate small PR from origin/master that you open yourself." Found while checking upstream master's `test_cuda_pic_preallocation` SIGSEGV against the fork (`docs/state.md`, Netflix/vmaf#1573 hunk (a) row).
- [ADR-1032](1032-vmaf-init-double-init-guard-vmaf-close-pointer-contract.md) (the guard, Fix 1 of three).
- [docs/api/index.md](../api/index.md) "ABI stability" (the stable-surface promise).
- Upstream Netflix/vmaf `2f92791c`: `libvmaf/src/libvmaf.c` `vmaf_init()`, `libvmaf/tools/vmaf.c`, `libvmaf/test/test_context.c`.
- `docs/state.md` `T-VMAF-INIT-READS-INCOMING-HANDLE-2026-09-30`.
