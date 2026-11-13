---
paths:
  - core/src/libvmaf.c
  - core/include/libvmaf/libvmaf.h
invariant: vmaf_init never reads output handle; subsystems init owns framesync lifecycle; output writers test ferror.
area: engine-and-runtime
---
<!-- markdownlint-disable MD013 MD060 -->
# Context lifecycle, initialization, and dispatch callbacks

- **`vmaf_init` never reads `*vmaf`** ([ADR-1396](../../docs/adr/1396-vmaf-init-output-only-handle.md),
  superseding ADR-1032 Fix 1): it sets `*vmaf = NULL` on entry and
  `*vmaf = v` on success. Upstream callers pass uninitialised handle
  (`VmafContext *vmaf;`), so check of incoming value fails them at
  random. Do not reintroduce `if (*vmaf) return -EINVAL;` guard.
  `test_vmaf_init_ignores_the_incoming_handle` (`test/test_context.c`) fails
  if one comes back.
- **`vmaf_init` cpumask narrowing uses explicit `(unsigned)` cast**
  (fork-local, round-5 `-fsanitize=integer` sweep):
  `vmaf_set_cpu_flags_mask((unsigned)(~cfg.cpumask))` in
  [`src/libvmaf.c`](../src/libvmaf.c). Cast is deliberate: all
  defined CPU flag bits fit in 6 bits; high 32 bits of
  `uint64_t cpumask` complement are always zero for any valid input.
  Never remove explicit cast.
- **Output writers return `ferror(outfile) ? -EIO : 0`.**
  `vmaf_write_output_{xml,json,csv,sub}` in
  [src/output.c](../src/output.c) use single tail `return` that
  checks `ferror(outfile)` — per [ADR-0119](../../docs/adr/0119-cli-precision-default-revert.md).
  Any upstream patch changing tail to bare `return 0`
  must be merged so fork's `ferror` check survives.
  Thread-locale bracket from [ADR-0137](../../docs/adr/0137-thread-local-locale-for-numeric-io.md)
  is `push_c()` at entry → body → `pop()` before `ferror`
  check; dropping `pop()` leaks `locale_t` on POSIX,
  leaves calling thread locked to `"C"` on Windows.

- **Dispatch choice reads callbacks AFTER init** (`init_before_dispatch()`,
  `src/libvmaf.c`). `read_pictures_cuda_submit_current()` and
  `read_pictures_dispatch_one()` init extractor with `submit()` +
  `collect()` before picking async path vs `extract()`: GPU motion twins'
  `init()` swaps in `extract()` under `motion_force_zero`. Choice first =
  first frame calls cleared `submit()` -> SIGSEGV
  (`T-GPU-MOTION-FORCE-ZERO-FIRST-FRAME-SEGV-2026-09-30`). Keep init ahead of
  decision; `test_cuda_kernel_source_contract.py` pins order.
- **`vmaf_ctx_subsystems_init` owns init/teardown chain** for framesync →
  feature collector → extractor vector → thread pools; new subsystem gets
  new label in that function, not in `vmaf_init`.
