# AGENTS.md — libvmaf/include/libvmaf

Orientation for agents working on libvmaf's public C API headers.
Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

Public headers consumed by external callers (FFmpeg patches, MCP
server, Python bindings, downstream tools). Layout:

```text
libvmaf/include/libvmaf/
  libvmaf.h              # core: VmafContext, VmafConfiguration, score paths
  picture.h              # VmafPicture
  model.h                # VmafModel
  feature.h              # VmafFeatureExtractor (selection + collection)
  dnn.h                  # tiny-AI session API
  libvmaf_cuda.h         # CUDA backend
  libvmaf_sycl.h         # SYCL backend
  libvmaf_hip.h          # HIP / AMD-ROCm backend (scaffold only)
  # libvmaf_vulkan.h removed per ADR-0726 (Vulkan backend dropped)
```

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../AGENTS.md)).
- **ABI is additive only.** Configuration / Picture-configuration
  structs grow at the end. Zero-initialised callers from any prior
  version must continue to compile + run with default behaviour.
  This is a project-wide invariant, not a per-header one.
- **Never remove or rename a public symbol** without an ADR + a
  matching `ffmpeg-patches/` update per CLAUDE.md §12 r14. The
  `enabled libvmaf*` `check_pkg_config` lines in
  `ffmpeg-patches/000?-*.patch` probe specific symbol names.
- **Doxygen on every entry point.** `@return` lists every error
  path, including the `-ENOSYS` "built-without-backend" case where
  applicable.

## GPU backend public-API template

When adding a new GPU backend (Metal, DirectML, OpenCL, …), follow
the shape the existing four backends already share. The recipe lives
at [`docs/development/gpu-backend-template.md`](../../../docs/development/gpu-backend-template.md):

- The shared lifecycle (`vmaf_<backend>_state_init` /
  `_import_state` / `_state_free`) — every backend ships these.
- Optional sections (`_list_devices`, `_available`, picture
  preallocation, zero-copy hwaccel import) — pick the ones the
  backend actually needs.
- Doxygen + ABI stability conventions.

The template is **doc-pattern, not codegen** (the 2026-05-02 audit
found 95 % of each header is backend-specific feature surface;
codegen would shave ~10 % at the cost of a build-system Python
dependency — ADR-0239's "headers second" PR consciously ships the
template doc + AGENTS guidance instead).

The matching internal-side companion files
(`libvmaf/src/<backend>/`) follow their own pattern; the
backend-agnostic `gpu_picture_pool.{c,h}` round-robin
(ADR-0239) is the only currently-extracted shared internal helper.

## Rebase-sensitive invariants

- **Every declaration in this directory must carry `VMAF_EXPORT`**
  ([ADR-0379](../../../docs/adr/0379-libvmaf-symbol-visibility.md),
  Research-0092). `libvmaf.so` is built with `-fvisibility=hidden`
  globally; a public declaration without `VMAF_EXPORT` is silently
  hidden in the DSO. New entry points: add `VMAF_EXPORT` to the
  function declaration in the appropriate header here before merging.
  `macros.h` defines the macro and is included by `libvmaf.h`, which
  all backend headers already include — no extra `#include` is needed
  for headers that transitively pull in `libvmaf.h`. Verify with:
  ```bash
  nm -D --defined-only build/src/libvmaf.so.3.0.0 | grep ' [TW] ' | grep -v ' vmaf_' | wc -l
  # Must print 0
  ```

- **Public surface stability**: the backend headers landed in
  this order — `libvmaf_cuda.h` (Netflix upstream, baseline),
  `libvmaf_sycl.h` (fork ADR-0152, T1-7 — SYCL backend scaffold),
  `libvmaf_vulkan.h` (fork ADR-0175, T5-1 — REMOVED in ADR-0726),
  `libvmaf_hip.h` (fork ADR-0212 / T7-10 — HIP scaffold).
  An upstream sync that touches `libvmaf_cuda.h` is *expected*; one
  that touches `libvmaf_sycl.h` or `libvmaf_hip.h` would be a mis-merge.
- **Picture preallocation surfaces**: CUDA's
  `VmafCudaPicturePreallocationMethod` ships
  `NONE / DEVICE / HOST / HOST_PINNED`; SYCL + Vulkan ship
  `NONE / HOST / DEVICE` (no `HOST_PINNED` — VMA's
  `AUTO_PREFER_HOST` isn't pinned in the CUDA sense). New backends
  follow the SYCL/Vulkan three-method shape; do not introduce a
  fourth method without an ADR.
- **`picture.h` v1 is frozen for the v2 deprecation window**
  ([ADR-0928](../../../docs/adr/0928-vmaf-picture-v2-explicit-backend-state.md)).
  Do not add fields to `VmafPicture` v1 — additive growth lands on
  `VmafPicture2` (`picture_v2.h`) instead. The v1 struct is a
  museum piece for ~12 months and is removed when SONAME bumps
  from `libvmaf.so.3` to `.4` at VMAFX v4.0.0. `picture_v2.h` is
  declared but not yet linked in cycle N; entry points return
  `-ENOSYS` until the cycle-N+1 implementation PR lands.
