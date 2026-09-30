- **`vmaf_init()` accepts an uninitialised handle again, as upstream libvmaf
  does.** Since ADR-1032 it returned `-EINVAL` whenever `*vmaf` was not NULL.
  Callers written against upstream, whose own CLI and tests declare
  `VmafContext *vmaf;` without an initialiser, failed at random depending on
  what the stack held. Upstream's `test_context.c` failed 3 of 3 runs.
  `vmaf_init()` no longer reads `*vmaf`: it sets it to NULL on entry and to
  the new context on success (ADR-1396). A handle that still holds an open
  context is now overwritten instead of rejected; close it first.
