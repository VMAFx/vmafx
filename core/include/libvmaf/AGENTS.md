# AGENTS.md — core/include/libvmaf

Orientation for agents working on libvmaf's public C API headers.
Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

Public headers consumed by external callers (FFmpeg patches, MCP
server, Python bindings, downstream tools). Layout:

```text
core/include/libvmaf/
  libvmaf.h              # core: VmafContext, VmafConfiguration, score paths
  picture.h              # VmafPicture
  model.h                # VmafModel
  feature.h              # VmafFeatureExtractor (selection + collection)
  dnn.h                  # tiny-AI session API
  libvmaf_cuda.h         # CUDA backend
  libvmaf_sycl.h         # SYCL backend
  libvmaf_hip.h          # HIP / AMD-ROCm backend (scaffold only)
  libvmaf_metal.h        # Metal backend (Apple Silicon / macOS; scaffold only)
  libvmaf_mcp.h          # MCP server C-API bridge (query scores over JSON-RPC)
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
(`core/src/<backend>/`) follow their own pattern; the
backend-agnostic `gpu_picture_pool.{c,h}` round-robin
(ADR-0239) is the only currently-extracted shared internal helper.

## Rebase-sensitive invariants

- **Include guards use the `LIBVMAF_<BASENAME>_H` pattern**
  ([ADR-0972](../../../docs/adr/0972-public-header-iso-reserved-guards.md),
  [Research-0762](../../../docs/research/0762-public-header-iso-reserved-guards-2026-05-31.md)).
  Identifiers starting with `__` or `_` followed by an uppercase
  letter are reserved by C17 §7.1.3 and banned by SEI CERT DCL37-C.
  Clang's `-Wreserved-identifier` rejects them. Upstream Netflix/vmaf
  still ships the old `__VMAF_*__` form in headers it owns
  (`libvmaf.h`, `picture.h`, `feature.h`, `model.h`, `libvmaf_cuda.h`);
  an upstream sync that re-introduces those identifiers must be
  rewritten on import to keep our `LIBVMAF_<BASENAME>_H` form.
  `macros.h`, `vmaf_assert.h`, `dnn.h`, `libvmaf_sycl.h`,
  `libvmaf_hip.h`, `libvmaf_metal.h`, `libvmaf_mcp.h` are fork-only
  and not subject to upstream churn here.

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
  implemented and linked as of cycle N+1 (`core/src/picture_v2.c`);
  all five entry points are live in libvmaf.so.

- **Doxygen-clean public API** ([ADR-0953](../../../docs/adr/0953-doxygen-public-api-clean.md)):
  every header in this directory must produce **zero warnings** when
  run through `doxygen core/doc/Doxyfile.public-api`. The CI workflow
  `.github/workflows/doxygen-public-api.yml` builds the doxygen tree
  on every PR touching this directory and publishes the warning log
  as a build artifact. Patterns to avoid because they trigger
  warnings (and which the audit closed out):
  - **`@field name desc` for struct members** is not a doxygen
    command — use per-member inline `/**< desc */`.
  - **`@ref function_name` from a struct doc-block** does not resolve
    cross-symbol — use backtick literals (`vmaf_picture_alloc`)
    instead.
  - **Functions without `@param` per parameter** or **without
    `@return`** trigger `WARN_NO_PARAMDOC` / incomplete-doc warnings.
  - **Multi-name declarations** (`unsigned w[3], h[3];`) attach the
    inline doc to one symbol only — split into one declaration per
    line so each symbol carries its own doc.

## `enum VmafBackend` / `vmaf_context_get_backend` rebase invariant

([ADR-0804](../../../docs/adr/0804-vmaf-context-get-backend.md))

The `VMAF_BACKEND_*` enumerator values are **stable and append-only**.
Do not renumber existing values; do not reuse a retired value; only
append new members at the end (before a possible future sentinel).

Each `vmaf_<backend>_import_state()` implementation must set
`vmaf->active_backend` to the matching `VMAF_BACKEND_*` constant.
If a new backend is added, both the enum member **and** the
`import_state` assignment must land in the same PR.
