<!-- markdownlint-disable MD013 MD060 -->
<!--
  research-0752-phase-4b8-c-abi-break-scoping.md
  Phase 4b.8 — libvmaf C ABI break: full scoping digest
  Companion to ADR-0767.
-->

# Research-0752: Phase 4b.8 — libvmaf C ABI Break Scoping

**Status**: Draft
**Date**: 2026-05-29
**Companion ADR**: [ADR-0767](../adr/0767-phase-4b8-c-abi-break-scoping.md)

---

## 1. Current C ABI surface — complete symbol inventory

### 1.1 Core context + lifecycle (`libvmaf.h`)

| Symbol | Signature | Notes |
|--------|-----------|-------|
| `vmaf_init` | `int (VmafContext**, VmafConfiguration)` | pass-by-value cfg struct |
| `vmaf_close` | `int (VmafContext*)` | |
| `vmaf_use_features_from_model` | `int (VmafContext*, VmafModel*)` | |
| `vmaf_use_features_from_model_collection` | `int (VmafContext*, VmafModelCollection*)` | |
| `vmaf_use_feature` | `int (VmafContext*, const char*, VmafFeatureDictionary*)` | transfers dict ownership |
| `vmaf_import_feature_score` | `int (VmafContext*, const char*, double, unsigned)` | |
| `vmaf_read_pictures` | `int (VmafContext*, VmafPicture*, VmafPicture*, unsigned)` | NULL flush |
| `vmaf_score_at_index` | `int (VmafContext*, VmafModel*, double*, unsigned)` | |
| `vmaf_score_at_index_model_collection` | `int (VmafContext*, VmafModelCollection*, VmafModelCollectionScore*, unsigned)` | |
| `vmaf_feature_score_at_index` | `int (VmafContext*, const char*, double*, unsigned)` | |
| `vmaf_score_pooled` | `int (VmafContext*, VmafModel*, VmafPoolingMethod, double*, unsigned, unsigned)` | |
| `vmaf_score_pooled_model_collection` | `int (VmafContext*, VmafModelCollection*, VmafPoolingMethod, VmafModelCollectionScore*, unsigned, unsigned)` | |
| `vmaf_feature_score_pooled` | `int (VmafContext*, const char*, VmafPoolingMethod, double*, unsigned, unsigned)` | |
| `vmaf_preallocate_pictures` | `int (VmafContext*, VmafPictureConfiguration)` | pass-by-value cfg |
| `vmaf_fetch_preallocated_picture` | `int (VmafContext*, VmafPicture*)` | |
| `vmaf_write_output` | `int (VmafContext*, const char*, VmafOutputFormat)` | |
| `vmaf_write_output_with_format` | `int (VmafContext*, const char*, VmafOutputFormat, const char*)` | ADR-0119 |
| `vmaf_version` | `const char* (void)` | |

**Struct `VmafConfiguration`**: `{VmafLogLevel, unsigned n_threads, unsigned n_subsample, uint64_t cpumask, uint64_t gpumask}` — pass-by-value; adding fields is a binary break.

**Struct `VmafPictureConfiguration`**: anonymous sub-struct `{w,h,bpc,pix_fmt}` + `pic_cnt` — same pass-by-value break risk.

### 1.2 Picture surface (`picture.h`)

| Symbol | Notes |
|--------|-------|
| `vmaf_picture_alloc` | host-alloc; takes `pix_fmt, bpc, w, h` individually |
| `vmaf_picture_unref` | ref-counted release |
| `VmafPicture` (struct) | `{pix_fmt, bpc, w[3], h[3], stride[3], data[3], VmafRef*, priv}` — `priv` is opaque-ish but public |

### 1.3 Model surface (`model.h`)

| Symbol | Notes |
|--------|-------|
| `vmaf_model_load` | loads by version string (built-in model registry) |
| `vmaf_model_load_from_path` | loads from filesystem path |
| `vmaf_model_feature_overload` | transfers `VmafFeatureDictionary` ownership |
| `vmaf_model_destroy` | void return — inconsistent with other `destroy/free` patterns |
| `vmaf_model_collection_load` | |
| `vmaf_model_collection_load_from_path` | |
| `vmaf_model_collection_feature_overload` | |
| `vmaf_model_collection_destroy` | void return |
| `vmaf_model_version_next` | iterator; returns `const void*` — opaque pattern |
| `VmafModelKind` enum | `SVM=0, DNN_FR=1, DNN_NR=2, DNN_FILTER=3` |
| `VmafModelFlags` enum | `DEFAULT=0, DISABLE_CLIP, ENABLE_TRANSFORM, DISABLE_TRANSFORM` |
| `VmafModelConfig` struct | `{const char *name, uint64_t flags}` |
| `VmafModelCollectionScore` struct | nested `bootstrap.{bagging_score, stddev, ci.p95.{lo,hi}}` |

### 1.4 Feature dictionary surface (`feature.h`)

| Symbol | Notes |
|--------|-------|
| `vmaf_feature_dictionary_set` | key/value string pairs; takes `**dict` |
| `vmaf_feature_dictionary_free` | takes `**dict` |

### 1.5 CUDA backend (`libvmaf_cuda.h`)

| Symbol | Notes |
|--------|-------|
| `vmaf_cuda_state_init` | takes `VmafCudaConfiguration` by value — `{void *cu_ctx}` |
| `vmaf_cuda_state_free` | |
| `vmaf_cuda_import_state` | |
| `vmaf_cuda_preallocate_pictures` | `VmafCudaPictureConfiguration` by value |
| `vmaf_cuda_fetch_preallocated_picture` | |
| `VmafCudaPicturePreallocationMethod` enum | 3 values |

### 1.6 SYCL backend (`libvmaf_sycl.h`)

| Symbol | Notes |
|--------|-------|
| `vmaf_sycl_state_init` | `VmafSyclConfiguration` by value |
| `vmaf_sycl_import_state` | |
| `vmaf_sycl_preallocate_pictures` | |
| `vmaf_sycl_picture_fetch` | (note: naming inconsistency vs CUDA `fetch_preallocated_picture`) |
| `vmaf_sycl_init_frame_buffers` | |
| `vmaf_sycl_get_frame_buffers` | |
| `vmaf_sycl_wait_compute` | |
| `vmaf_read_pictures_sycl` | lives in `libvmaf.h` namespace but SYCL-specific |
| `vmaf_flush_sycl` | |
| `vmaf_sycl_dmabuf_import` | |
| `vmaf_sycl_dmabuf_free` | void return |
| `vmaf_sycl_import_va_surface` | |
| `vmaf_sycl_upload_plane` | |
| `vmaf_sycl_import_d3d11_surface` | Windows only; `#ifdef _WIN32` |
| `vmaf_sycl_profiling_enable` | |
| `vmaf_sycl_profiling_disable` | void return |
| `vmaf_sycl_profiling_print` | void return |
| `vmaf_sycl_profiling_get_string` | caller must `free()` result |
| `vmaf_sycl_state_free` | takes `**` |
| `vmaf_sycl_list_devices` | |

### 1.7 Vulkan backend (`libvmaf_vulkan.h`)

| Symbol | Notes |
|--------|-------|
| `vmaf_vulkan_available` | |
| `vmaf_vulkan_state_init` | `VmafVulkanConfiguration` by value |
| `vmaf_vulkan_state_init_external` | `VmafVulkanExternalHandles` by value |
| `vmaf_vulkan_state_max_outstanding_frames` | |
| `vmaf_vulkan_import_state` | |
| `vmaf_vulkan_preallocate_pictures` | |
| `vmaf_vulkan_picture_fetch` | |
| `vmaf_vulkan_state_free` | takes `**` |
| `vmaf_vulkan_list_devices` | |
| `vmaf_vulkan_import_image` | zero-copy frame import; ADR-0184/ADR-0186 |
| `vmaf_vulkan_wait_compute` | |
| `vmaf_vulkan_read_imported_pictures` | |

### 1.8 HIP backend (`libvmaf_hip.h`) — scaffold only

| Symbol | Notes |
|--------|-------|
| `vmaf_hip_available` | |
| `vmaf_hip_state_init` | `VmafHipConfiguration` by value |
| `vmaf_hip_import_state` | |
| `vmaf_hip_state_free` | takes `**` |
| `vmaf_hip_list_devices` | |

### 1.9 Metal backend (`libvmaf_metal.h`)

| Symbol | Notes |
|--------|-------|
| `vmaf_metal_available` | |
| `vmaf_metal_state_init` | `VmafMetalConfiguration` by value |
| `vmaf_metal_state_init_external` | `VmafMetalExternalHandles` by value |
| `vmaf_metal_import_state` | |
| `vmaf_metal_state_free` | takes `**` |
| `vmaf_metal_list_devices` | |
| `vmaf_metal_picture_import` | IOSurface zero-copy; ADR-0423 |
| `vmaf_metal_wait_compute` | |
| `vmaf_metal_read_imported_pictures` | |

### 1.10 DNN surface (`dnn.h`)

| Symbol | Notes |
|--------|-------|
| `vmaf_dnn_available` | |
| `vmaf_use_tiny_model` | attaches ONNX to VmafContext |
| `vmaf_dnn_set_codec_context` | codec conditioning; ADR-0518/0519 |
| `vmaf_dnn_set_resize_mode` | ADR-0550 |
| `vmaf_dnn_session_open` | standalone filter sessions |
| `vmaf_dnn_session_run_luma8` | |
| `vmaf_dnn_session_run_plane16` | |
| `vmaf_dnn_session_run` | generic named-tensor run |
| `vmaf_dnn_session_close` | void return |
| `vmaf_dnn_session_attached_ep` | diagnostic |
| `vmaf_dnn_verify_signature` | Sigstore; ADR-0211 |
| `VmafDnnDevice` enum | 12 values (0–11) |
| `VmafDnnResizeMode` enum | 4 values |
| `VmafDnnConfig` struct | `{device, device_index, threads, fp16_io}` |
| `VmafDnnInput` struct | |
| `VmafDnnOutput` struct | |
| `VmafDnnSession` opaque | |

### 1.11 MCP surface (`libvmaf_mcp.h`)

| Symbol | Notes |
|--------|-------|
| `vmaf_mcp_available` | |
| `vmaf_mcp_transport_available` | |
| `vmaf_mcp_init` | |
| `vmaf_mcp_start_sse` | |
| `vmaf_mcp_start_uds` | |
| `vmaf_mcp_start_stdio` | |
| `vmaf_mcp_stop` | |
| `vmaf_mcp_close` | void return, takes `**` |
| `VmafMcpTransport` enum | 3 values |
| `VmafMcpConfig` struct | |
| `VmafMcpSseConfig` struct | |
| `VmafMcpUdsConfig` struct | |
| `VmafMcpStdioConfig` struct | |

---

## 2. FFmpeg-patches entry points consumed

The 15 patches (`0001`–`0015`) in `ffmpeg-patches/` consume the following public symbols:

| Symbol | Patch(es) |
|--------|-----------|
| `vmaf_init`, `vmaf_close` | 0001..0015 (every filter) |
| `vmaf_read_pictures` | 0001, 0010 |
| `vmaf_use_features_from_model` | 0001..0015 |
| `vmaf_model_load`, `vmaf_model_load_from_path` | 0001..0015 |
| `vmaf_model_destroy` | 0001..0015 |
| `vmaf_score_pooled` | 0001..0015 |
| `vmaf_write_output` | implied by filter flush |
| `vmaf_picture_alloc`, `vmaf_picture_unref` | 0001..0015 |
| `vmaf_dnn_available`, `vmaf_use_tiny_model` | 0001 |
| `vmaf_cuda_state_init`, `vmaf_cuda_import_state` | 0010 |
| `vmaf_cuda_preallocate_pictures`, `vmaf_cuda_fetch_preallocated_picture` | 0010 |
| `vmaf_cuda_state_free` | 0010 |
| `vmaf_sycl_state_init`, `vmaf_sycl_import_state` | 0003, 0005 |
| `vmaf_sycl_init_frame_buffers`, `vmaf_sycl_wait_compute` | 0003, 0005 |
| `vmaf_sycl_import_va_surface`, `vmaf_sycl_profiling_*` | 0005 |
| `vmaf_read_pictures_sycl`, `vmaf_flush_sycl` | 0003, 0005 |
| `vmaf_sycl_state_free` | 0003, 0005 |
| `vmaf_vulkan_state_init`, `vmaf_vulkan_import_state` | 0004, 0006 |
| `vmaf_vulkan_import_image`, `vmaf_vulkan_wait_compute` | 0006 |
| `vmaf_vulkan_read_imported_pictures`, `vmaf_vulkan_state_free` | 0004, 0006 |
| `vmaf_hip_state_init`, `vmaf_hip_import_state`, `vmaf_hip_state_free` | 0011 |
| `vmaf_metal_state_init`, `vmaf_metal_import_state` | 0012, 0013 |
| `vmaf_metal_picture_import`, `vmaf_metal_wait_compute` | 0013 |
| `vmaf_metal_read_imported_pictures`, `vmaf_metal_state_free` | 0012, 0013 |
| `vmaf_metal_state_init_external` | 0013 (external-handle path) |

Any rename, signature change, or removal in the v4 break requires a corresponding patch update.

---

## 3. Proposed breaking changes for libvmaf.so.4 / v4.0.0

### 3.1 Remove `vmaf_write_output` (superseded by `vmaf_write_output_with_format`)

`vmaf_write_output` is functionally subsumed by `vmaf_write_output_with_format` (ADR-0119). In v4 it should be removed, with a trivial inline wrapper available for the transition window if desired (deprecated header shim in the v4.0 series).

### 3.2 Remove `vmaf_model_load` by version-string lookup

`vmaf_model_load(model, cfg, "vmaf_v0.6.1")` is a magic-string look-up into the built-in model registry. For v4, replace with:

```c
int vmaf_model_load_builtin(VmafModel **model, VmafModelConfig *cfg, const void *handle);
```

where `handle` comes from `vmaf_model_version_next`. This makes the API self-consistent (iterate, then load) and removes the fragile string→path resolution in the library. The iterator API already exists; v4 removes the bypass.

### 3.3 Change configuration structs from pass-by-value to pass-by-pointer

`VmafConfiguration`, `VmafPictureConfiguration`, `VmafCudaPictureConfiguration`, `VmafSyclPictureConfiguration`, `VmafVulkanPictureConfiguration`, and `VmafVulkanConfiguration` are all passed by value. Adding any field to any of these structs is a silent binary break even in a minor version. In v4:

```c
int vmaf_init(VmafContext **vmaf, const VmafConfiguration *cfg);
int vmaf_preallocate_pictures(VmafContext *vmaf, const VmafPictureConfiguration *cfg);
/* … same for all GPU configuration structs */
```

This is the standard convention used by every GPU backend added after the original API (SYCL state, Vulkan state, HIP state all take config by value but their `*Configuration` structs were small enough to avoid immediate pain). The v4 break is the forcing function to fix all of them atomically.

### 3.4 Normalize `destroy/free/close` return types

`vmaf_model_destroy`, `vmaf_model_collection_destroy`, `vmaf_dnn_session_close`, `vmaf_sycl_profiling_disable`, `vmaf_sycl_profiling_print`, `vmaf_sycl_dmabuf_free` all return `void`. All other lifecycle functions return `int`. In v4, align to `int` returns everywhere so callers can detect partial cleanup failures (double-free detection, CUDA stream drain failure, etc.).

### 3.5 Rename `vmaf_sycl_picture_fetch` → `vmaf_sycl_fetch_preallocated_picture`

Naming inconsistency vs the CPU and CUDA equivalents (`vmaf_fetch_preallocated_picture`, `vmaf_cuda_fetch_preallocated_picture`). Low risk, trivially scripted sed-patch for all consumers.

### 3.6 Remove `vmaf_read_pictures_sycl` and `vmaf_flush_sycl` from `libvmaf.h` namespace

These SYCL-specific entry points (`vmaf_read_pictures_sycl`, `vmaf_flush_sycl`) reside conceptually in the SYCL backend but were placed in the base header namespace. Move them to `libvmaf_sycl.h` for consistency.

### 3.7 Introduce `VMAF_MODEL_FLAG_DEPRECATED_CLIP` and remove `VMAF_MODEL_FLAG_DISABLE_CLIP`

The `DISABLE_CLIP` flag was introduced for SVM model tuning. Under the DNN surface (ADR-0519), score clipping is irrelevant. For v4, merge clip control into a single `VmafModelOptions` struct attached to `VmafModelConfig`, rather than a bitmask, enabling boolean clarity and future fields without bit-width pressure.

### 3.8 Expose `VmafPicture.priv` size contract

`VmafPicture.priv` is typed `void*` and currently opaque. In v4, rename to `_reserved` and add a `size_t priv_capacity` field to the struct to make the contract visible to embedders (e.g. ffmpeg allocates its own `LIBVMAFContext` which embeds `VmafPicture` objects directly).

### 3.9 Add `vmaf_context_get_backend` query

Currently there is no public way to ask a live `VmafContext` which GPU backend (if any) is active. v4 adds:

```c
typedef enum VmafBackend {
    VMAF_BACKEND_CPU     = 0,
    VMAF_BACKEND_CUDA    = 1,
    VMAF_BACKEND_SYCL    = 2,
    VMAF_BACKEND_VULKAN  = 3,
    VMAF_BACKEND_HIP     = 4,
    VMAF_BACKEND_METAL   = 5,
} VmafBackend;

int vmaf_context_get_backend(const VmafContext *vmaf, VmafBackend *out);
```

This is a v4 addition (not a removal), but it replaces the current pattern where callers must track the backend externally.

---

## 4. Major version bump: libvmaf.so.4 / VMAFx v4.0.0

- `soname` changes from `libvmaf.so.3` → `libvmaf.so.4`.
- `meson.build` version block changes to `4.0.0`; `VMAF_API_VERSION_MAJOR` macro becomes `4`.
- `release-please` already targets `v3.x.y-lusoris.N`; in the v4 PR it must be bumped to `v4.0.0-lusoris.1` with a commit message `feat!:` (BREAKING CHANGE footer) to trigger the major release.
- The library continues to install as `libvmaf.so` (unversioned symlink) and `libvmaf.so.4`; the `.3.x.y` versioned file disappears from new builds. Distro packagers must declare `Breaks: libvmaf3`.

---

## 5. ffmpeg-patches rewrite plan

Per CLAUDE.md §12 r14, all 15 patches must be updated in the same PR as the ABI break.

**Approach**: the patches are applied as a stack against the `n8.2` base (per ADR-0717 / PR #36 which pinned ffmpeg n8.2). A v4 ABI break triggers a rebase of the entire stack:

1. `scripts/ffmpeg-patches/rebase-onto-n8.2.sh` (existing helper) — used to verify the current state.
2. Each changed symbol listed in §2 must be updated in the relevant patch: renamed functions, pointer-ified config structs, void→int return types.
3. `0001` and all filters that call `vmaf_write_output` directly switch to `vmaf_write_output_with_format`.
4. `0003 / 0005` adapt to the moved `vmaf_read_pictures_sycl` / `vmaf_flush_sycl` include path.
5. All config-by-pointer changes require `&cfg` on the callsite.
6. After rebase, run the series-replay gate (`git am --3way` in a clean n8.2 checkout) — per CLAUDE.md §12 r14, `git apply --check` is an insufficient gate.

The `/refresh-ffmpeg-patches` skill is the correct tool to execute this once the v4 header changes are committed to master.

---

## 6. Migration guide outline (libvmaf.so.3 → libvmaf.so.4)

| Change | v3 | v4 |
|--------|----|----|
| `vmaf_init` cfg arg | `VmafConfiguration` by value | `const VmafConfiguration *` |
| `vmaf_preallocate_pictures` cfg arg | `VmafPictureConfiguration` by value | `const VmafPictureConfiguration *` |
| All GPU `*Configuration` cfg args | by value | by const-pointer |
| `vmaf_write_output` | available | removed; use `vmaf_write_output_with_format(..., NULL)` |
| `vmaf_model_load` | magic string lookup | removed; use `vmaf_model_version_next` + `vmaf_model_load_builtin` |
| `vmaf_model_destroy`, etc. | `void` return | `int` return |
| `vmaf_sycl_picture_fetch` | old name | `vmaf_sycl_fetch_preallocated_picture` |
| `vmaf_read_pictures_sycl` | in `libvmaf.h` | moved to `libvmaf_sycl.h` |
| `vmaf_flush_sycl` | in `libvmaf.h` | moved to `libvmaf_sycl.h` |
| `VmafPicture.priv` | `void*` opaque | `void *_reserved` + `size_t priv_capacity` |
| New: `vmaf_context_get_backend` | not available | added |
| `soname` | `libvmaf.so.3` | `libvmaf.so.4` |
| `pkg-config --modversion libvmaf` | `3.0.0` | `4.0.0` |

**Compile-time detection**: callers can `#if VMAF_API_VERSION_MAJOR >= 4` for dual-version source compatibility.

**Symbol-level migration script** (to be provided in `scripts/migration/v3-to-v4.sed`):

```sed
s/vmaf_write_output(\([^,]*\), \([^,]*\), \([^)]*\))/vmaf_write_output_with_format(\1, \2, \3, NULL)/g
s/vmaf_sycl_picture_fetch/vmaf_sycl_fetch_preallocated_picture/g
s/vmaf_model_destroy(/vmaf_model_destroy(/g  # int return — add (void) cast for ignored return
```

---

## 7. Test plan

### 7.1 Netflix golden assertions — unaffected

The 3 Netflix golden-data test pairs (`src01`, `checkerboard_1px`, `checkerboard_10px`) are CLI tests exercised by `make test-netflix-golden`. They go through the CLI binary, not the C ABI directly. A correct v4 implementation with the CLI adapted will continue to produce identical scores. These assertions are never modified.

### 7.2 C unit tests in `core/test/`

The existing C unit tests in `core/test/` call the public C API directly. Each test file that calls any renamed/removed entry point will fail to compile against the v4 headers — this is the intended compile-time signal that the PR is complete. Test files must be updated in the same PR.

### 7.3 New v4 ABI smoke tests

A new test suite `core/test/test_vmaf_v4_api.c` should cover:

- `vmaf_init` with pointer-style config.
- `vmaf_write_output_with_format` replaces the v3 `vmaf_write_output` path.
- `vmaf_model_version_next` + `vmaf_model_load_builtin` round-trip.
- `vmaf_context_get_backend` returns `VMAF_BACKEND_CPU` on a default-init context.
- `void→int` return paths return `0` on success.

### 7.4 ffmpeg-patches series replay gate

```bash
git -C /path/to/ffmpeg-8 reset --hard n8.2
for p in ffmpeg-patches/0001-*.patch ffmpeg-patches/0002-*.patch ...; do
    git -C /path/to/ffmpeg-8 am --3way "$p" || break
done
make -C /path/to/ffmpeg-8 -j$(nproc) libavfilter/vf_libvmaf.o
```

Must compile clean against the v4 header install path.

### 7.5 Cross-backend parity gate

After the ABI break, run `/cross-backend-diff` on the Netflix golden 576x324 pair across all backends (CPU, CUDA, SYCL, Vulkan). The per-frame per-feature ULP divergence must remain within the tolerances established by ADR-0214. The config-by-pointer change must not introduce any numeric delta vs the v3 baseline.

---

## 8. Scope exclusions (DO NOT implement in this PR)

- No changes to internal (non-`VMAF_EXPORT`) symbols.
- No changes to the Python harness (`compat/python-vmaf/`); it calls the CLI, not the C ABI.
- No changes to `.onnx` model files or the model registry format.
- No changes to `meson_options.txt` build flags — those are addressed in a separate Phase 4b PR.
- The `vmaf_mcp_*` surface is young enough that the config-by-pointer rule is already followed internally; no MCP symbol renames are needed.
- The HIP backend is scaffold-only; no HIP-specific changes beyond config-ptr alignment.
