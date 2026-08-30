<!-- markdownlint-disable MD060 -->
# Research-0093: Vulkan GCC 16 `-Wreturn-mismatch` build-break root-cause

**Date**: 2026-05-10
**Branch**: fix/vulkan-gcc16-return-mismatch
**Companion ADR**: [ADR-0376](../adr/0376-vulkan-void-to-int-buffer-invalidate.md)

## Summary

GCC 16 promotes `-Wreturn-mismatch` from a warning to a hard error. Four sites in
two Vulkan feature-extractor files had `return <int-expr>;` inside `static void`
functions:

| File | Line (pre-fix) | Function | Return expression |
|------|---------------|----------|-------------------|
| `float_ansnr_vulkan.c` | 299 | `reduce_partials` | `return err_inv;` |
| `float_ansnr_vulkan.c` | 302 | `reduce_partials` | `return err_inv;` |
| `cambi_vulkan.c` | 884 | `cambi_vk_readback_image` | `return err_inv_img;` |
| `cambi_vulkan.c` | 904 | `cambi_vk_readback_mask` | `return err_inv_mask;` |

All four sites guard calls to `vmaf_vulkan_buffer_invalidate()`, a coherency-flush
operation that makes GPU-written host-mapped memory visible to the CPU before the
readback pixel-copy loop begins. Under GCC 14–15 these `return <int>;` inside a
`void` function compiled without error (compiler treated them as `return;` and
discarded the value, silently swallowing the error code).

## Root cause

The original author wrote early-return guard clauses for the buffer-invalidate
call and intended the functions to be `static int`, but forgot to change the
return type in the signature. GCC pre-16 was permissive about `return <value>`
inside `void` functions as a non-standard extension; GCC 16 closes this gap.

## Risk profile

The coherency-flush failure path is rare in normal operation (a flush failure
indicates a driver-level fault). However, proceeding past a failed flush and
reading from the host-mapped buffer exposes the caller to stale GPU-written data,
which would produce silently incorrect VMAF scores without any error surface to
the user. The fix is correct and does not change behaviour on a functioning driver.

## Fix

`static void` → `static int` + `return 0` at end of function body + call-site
error propagation (standard fork pattern). No NOLINT, no test weakening.

## Build verification

`meson setup build-vk -Denable_cuda=false -Denable_sycl=false -Denable_vulkan=true`
followed by `ninja -C build-vk` on GCC 16.1.1 exits 0 after this fix. The
relevant object files (`feature_vulkan_float_ansnr_vulkan.c.o`,
`feature_vulkan_cambi_vulkan.c.o`) are rebuilt from the modified sources.

## Applicability to upstream

The upstream Netflix/vmaf repo does not have Vulkan feature extractors; these
files are 100% fork-local. No upstream port needed.
