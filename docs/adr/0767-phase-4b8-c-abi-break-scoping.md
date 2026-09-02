<!-- markdownlint-disable MD013 MD060 -->
# ADR-0767: Phase 4b.8 — libvmaf C ABI Break for VMAFx v4.0.0

- **Status**: Proposed
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `api`, `abi`, `phase4b`, `breaking-change`, `v4`, `ffmpeg-patches`, `fork-local`

## Context

The libvmaf C ABI shipped under `libvmaf.so.3` has accumulated several structural problems that
cannot be corrected without a binary break:

1. **Pass-by-value configuration structs.** `VmafConfiguration`, `VmafPictureConfiguration`, and
   every GPU backend `*Configuration` type cross function call boundaries by value. Any future field
   addition (new backend selector, precision flag, etc.) silently breaks binary compatibility without
   a version bump. This was acceptable when the API was small; with 5 GPU backends and the DNN and
   MCP surfaces, the risk is no longer theoretical.

2. **`vmaf_write_output` is superseded.** `vmaf_write_output_with_format` (ADR-0119) fully replaces
   the fixed-format variant. Keeping both creates confusion; v3 callers must check which to call.

3. **`vmaf_model_load` by magic string.** The version-string lookup bypasses the
   `vmaf_model_version_next` iterator that was added precisely to make model enumeration safe. It
   is also the only entry point that embeds the model registry path resolution inside the library;
   the path is an implicit dependency.

4. **Inconsistent `void` vs `int` return types.** Several lifecycle/free functions return `void`
   while all others return `int`. Callers cannot detect GPU drain errors on `vmaf_model_destroy` or
   `vmaf_dnn_session_close`.

5. **`vmaf_read_pictures_sycl` / `vmaf_flush_sycl` in the wrong namespace.** These SYCL-specific
   entry points were placed in `libvmaf.h` rather than `libvmaf_sycl.h`, creating an include
   dependency on the SYCL backend even for CPU-only callers.

6. **Naming inconsistency.** `vmaf_sycl_picture_fetch` vs `vmaf_cuda_fetch_preallocated_picture`
   vs `vmaf_fetch_preallocated_picture` — three different naming patterns for the same operation.

The VMAFx Phase 4b rebrand (memory note: 2026-05-27/28) targets a `v4.0.0` major release with
aggressive modernization. The user authorized "BREAK C ABI" explicitly. This is the design
decision for what to break, how, and when.

See the full scoping digest: [Research-0752](../research/research-0752-phase-4b8-c-abi-break-scoping.md).

## Decision

We will produce a `libvmaf.so.4` ABI at VMAFx v4.0.0 incorporating the following breaking changes,
all in a single PR:

1. **All configuration structs become `const *` parameters** across `vmaf_init`,
   `vmaf_preallocate_pictures`, and every GPU backend init/prealloc entry point.
2. **Remove `vmaf_write_output`**; replace all callsites with `vmaf_write_output_with_format(..., NULL)`.
3. **Remove `vmaf_model_load` (version-string overload)**; callers use `vmaf_model_version_next` +
   new `vmaf_model_load_builtin(model, cfg, handle)`.
4. **`void`-returning lifecycle/free functions become `int`-returning** (zero = success, negative
   errno = failure): `vmaf_model_destroy`, `vmaf_model_collection_destroy`,
   `vmaf_dnn_session_close`, `vmaf_sycl_profiling_disable`, `vmaf_sycl_profiling_print`,
   `vmaf_sycl_dmabuf_free`.
5. **Rename `vmaf_sycl_picture_fetch` → `vmaf_sycl_fetch_preallocated_picture`**.
6. **Move `vmaf_read_pictures_sycl` and `vmaf_flush_sycl` from `libvmaf.h` to `libvmaf_sycl.h`**.
7. **Add `vmaf_context_get_backend(const VmafContext*, VmafBackend*)` and `VmafBackend` enum**.
8. **All 15 `ffmpeg-patches/` patches are updated in the same PR** per CLAUDE.md §12 r14.

Implementation is explicitly deferred: this ADR is a design approval document. No source changes
land until the user reviews and approves the research digest.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| Fix incrementally within v3 soname | No major version bump; downstream consumers unaffected | Cannot change pass-by-value struct params without silent binary break; void returns stay forever; two parallel load paths remain | The Phase 4b mandate is aggressive modernization; half-measures accumulate debt rather than paying it down |
| Deprecate-then-remove (v3.x → v3.y → v4) | Softer migration story for external users | Requires maintaining both code paths for N release cycles; complicates the ffmpeg-patches series with conditional builds | The fork has a small, known external consumer surface; a clean single-version cut is simpler and the Phase 4b timeline absorbs it |
| Versioned symbol aliases (`vmaf_init_v3` → `vmaf_init_v4` ELF aliases) | Allows both ABIs in one `.so` | Doubles internal complexity; ELF versioning is poorly understood and rarely tested; linker script maintenance is ongoing debt | Overkill for a fork with a clear `v4` branch |
| C++ API layer instead of C break | Could use `std::expected`, RAII, proper namespacing | Breaks every C consumer outright; ffmpeg cannot use C++; the DNN and GPU backends require C linkage for cross-language use | The C ABI must remain the public surface per the VMAFx Phase 4b contract (preserve libvmaf.so ABI continuity across major versions, just bump major) |

## Consequences

- **Positive**: Configuration structs are extensible without future binary breaks. The API is
  internally consistent. External consumers get a single authoritative `v4` migration path with
  documented sed substitutions. The ffmpeg-patches series is in sync on day-1 of v4.
- **Negative**: Every downstream caller of `libvmaf.so.3` must recompile against v4 headers.
  The `ffmpeg-patches` series requires a full rebase (estimated: 1–2 hours with `/refresh-ffmpeg-patches`).
  Distro packagers must declare `Breaks: libvmaf3`.
- **Neutral / follow-ups**:
  - `scripts/migration/v3-to-v4.sed` to be created alongside implementation.
  - `docs/api/migration-v3-to-v4.md` human-readable migration guide to ship with the PR per
    CLAUDE.md §12 r10 (user-discoverable surface = public C API).
  - `core/test/test_vmaf_v4_api.c` smoke tests for all renamed/added entry points.
  - `soname` bump in `core/meson.build`: `3.0.0` → `4.0.0`.
  - `release-please` bootstrap commit requires `feat!:` type with `BREAKING CHANGE:` footer.
  - Netflix golden assertions in `python/test/` are unaffected (CLI tests, not ABI tests).

## References

- Research digest: [Research-0752](../research/research-0752-phase-4b8-c-abi-break-scoping.md)
- ADR-0119 (write_output_with_format)
- ADR-0184 / ADR-0186 (Vulkan import surface — uintptr_t handle convention to preserve)
- ADR-0211 (Sigstore verification)
- ADR-0214 (GPU parity gate — must remain passing)
- ADR-0518 / ADR-0519 (DNN codec conditioning)
- ADR-0550 (DNN resize mode)
- ADR-0700 (core/ rename — no ABI impact)
- ADR-0713 / ADR-0714 (Phase 4b node/operator — depend on v4 header install)
- CLAUDE.md §12 r14 (ffmpeg-patches lockstep rule)
- Memory note: "VMAFX rebrand plan (decided 2026-05-27)" — `project_vmafx_rebrand_plan.md`
- Memory note: "VMAFX Phase 4: language modernization (2026-05-28)" — `project_vmafx_phase4_language_modernization.md`
- Source: `req` — user authorized "BREAK C ABI" in the Phase 4b.8 planning message.
