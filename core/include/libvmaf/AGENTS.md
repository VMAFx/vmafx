# AGENTS.md — core/include/libvmaf

Orientation for agents on libvmaf public C API headers.
Parent: [../../AGENTS.md](../../AGENTS.md).

## Scope

Public headers for external callers (FFmpeg patches, MCP server,
Python bindings, downstream tools). Layout:

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
- **ABI is additive only.** Configuration / Picture-configuration structs
  grow at end. Zero-initialised callers from prior version must continue to
  compile + run with default behaviour. Project-wide invariant, not
  per-header.
- **Never remove or rename a public symbol** without ADR + matching
  `ffmpeg-patches/` update per CLAUDE.md §12 r14. `enabled libvmaf*`
  `check_pkg_config` lines in `ffmpeg-patches/000?-*.patch` probe specific
  symbol names.
- **Doxygen on every entry point.** `@return` lists every error path,
  including `-ENOSYS` "built-without-backend" case where applicable.

## GPU backend public-API template

When adding new GPU backend (Metal, DirectML, OpenCL, …), follow shape shared
by 4 existing backends. Recipe:
[`docs/development/gpu-backend-template.md`](../../../docs/development/gpu-backend-template.md):

- Shared lifecycle (`vmaf_<backend>_state_init` / `_import_state` /
  `_state_free`) — every backend ships these.
- Optional sections (`_list_devices`, `_available`, picture preallocation,
  zero-copy hwaccel import) — pick ones backend needs.
- Doxygen + ABI stability conventions.

Template = **doc-pattern, not codegen** (2026-05-02 audit: 95 % of each
header = backend-specific feature surface; codegen saves ~10 % at cost of
build-system Python dependency — ADR-0239 "headers second" PR ships template
doc + AGENTS guidance instead).

Matching internal-side companion files (`core/src/<backend>/`) follow own
pattern; backend-agnostic `gpu_picture_pool.{c,h}` round-robin (ADR-0239) =
only currently-extracted shared internal helper.

## Rebase-sensitive invariants

- **Include guards use the `LIBVMAF_<BASENAME>_H` pattern**
  ([ADR-0972](../../../docs/adr/0972-public-header-iso-reserved-guards.md),
  [Research-0762](../../../docs/research/0762-public-header-iso-reserved-guards-2026-05-31.md)).
  Identifiers starting with `__` or `_` followed by uppercase letter reserved
  by C17 §7.1.3, banned by SEI CERT DCL37-C. Clang `-Wreserved-identifier`
  rejects them. Upstream Netflix/vmaf still ships old `__VMAF_*__` form in
  owned headers (`libvmaf.h`, `picture.h`, `feature.h`, `model.h`,
  `libvmaf_cuda.h`); upstream sync re-introducing identifiers must be
  rewritten on import to keep `LIBVMAF_<BASENAME>_H` form.
  `macros.h`, `vmaf_assert.h`, `dnn.h`, `libvmaf_sycl.h`, `libvmaf_hip.h`,
  `libvmaf_metal.h`, `libvmaf_mcp.h` fork-only, not subject to upstream churn.
- **Every declaration in this directory must carry `VMAF_EXPORT`**
  ([ADR-0379](../../../docs/adr/0379-libvmaf-symbol-visibility.md),
  Research-0092). `libvmaf.so` built with `-fvisibility=hidden` globally;
  public declaration without `VMAF_EXPORT` silently hidden in DSO. New entry
  points: add `VMAF_EXPORT` to function declaration in header before merge.
  `macros.h` defines macro, included by `libvmaf.h` (all backend headers
  include `libvmaf.h`) — no extra `#include` needed for headers transitively
  pulling in `libvmaf.h`. Verify with:

  ```bash
  nm -D --defined-only build/src/libvmaf.so.3.0.0 | grep ' [TW] ' | grep -v ' vmaf_' | wc -l
  # Must print 0
  ```

- **Public surface stability**: backend headers landed in order —
  `libvmaf_cuda.h` (Netflix upstream baseline), `libvmaf_sycl.h` (fork
  ADR-0152, T1-7 — SYCL scaffold), `libvmaf_vulkan.h` (fork ADR-0175, T5-1 —
  REMOVED in ADR-0726), `libvmaf_hip.h` (fork ADR-0212 / T7-10 — HIP scaffold).
  Upstream sync touching `libvmaf_cuda.h` expected; sync touching
  `libvmaf_sycl.h` or `libvmaf_hip.h` = mis-merge.
- **Picture preallocation surfaces**: CUDA
  `VmafCudaPicturePreallocationMethod` ships
  `NONE / DEVICE / HOST / HOST_PINNED`; SYCL + Vulkan ship
  `NONE / HOST / DEVICE` (no `HOST_PINNED` — VMA `AUTO_PREFER_HOST` not
  pinned in CUDA sense). New backends follow SYCL/Vulkan 3-method shape;
  do not introduce fourth method without ADR.
- **`picture.h` v1 is frozen for the v2 deprecation window**
  ([ADR-0928](../../../docs/adr/0928-vmaf-picture-v2-explicit-backend-state.md)).
  Do not add fields to `VmafPicture` v1 — additive growth lands on
  `VmafPicture2` (`picture_v2.h`) instead. v1 struct museum piece for ~12
  months; removed when SONAME bumps `libvmaf.so.3` to `.4` at VMAFX v4.0.0.
  `picture_v2.h` implemented and linked as of cycle N+1
  (`core/src/picture_v2.c`); all 5 entry points live in libvmaf.so.
- **Doxygen-clean public API**
  ([ADR-0953](../../../docs/adr/0953-doxygen-public-api-clean.md)):
  every header in directory must produce **zero warnings** via
  `doxygen core/doc/Doxyfile.public-api`. CI workflow
  `.github/workflows/doxygen-public-api.yml` builds doxygen tree on PR
  touching directory; publishes warning log as build artifact. Patterns to
  avoid (trigger warnings, closed in audit):
  - **`@field name desc` for struct members** not doxygen command — use
    per-member inline `/**< desc */`.
  - **`@ref function_name` from a struct doc-block** does not resolve
    cross-symbol — use backtick literals (`vmaf_picture_alloc`) instead.
  - **Functions without `@param` per parameter** or **without `@return`**
    trigger `WARN_NO_PARAMDOC` / incomplete-doc warnings.
  - **Multi-name declarations** (`unsigned w[3], h[3];`) attach inline doc
    to 1 symbol only — split into 1 declaration per line so each symbol
    carries own doc.

## `enum VmafBackend` / `vmaf_context_get_backend` rebase invariant

([ADR-0804](../../../docs/adr/0804-vmaf-context-get-backend.md))

`VMAF_BACKEND_*` enumerator values = **stable and append-only**. Do not
renumber existing values; do not reuse retired value; only append new members
at end (before possible future sentinel).

Each `vmaf_<backend>_import_state()` implementation must set
`vmaf->active_backend` to matching `VMAF_BACKEND_*` constant. If new backend
added, enum member **and** `import_state` assignment must land in same PR.
