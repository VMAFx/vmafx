<!-- markdownlint-disable MD013 MD060 -->
# Research-0092: Symbol-Visibility Audit — Round-4 Bug-Hunt Finding

**Date**: 2026-05-10
**Found by**: `nm -D --defined-only` audit (angle 6 of the round-4 sanitizer sweep)
**Status**: Diagnosed — architectural fix required; deferred to a dedicated PR

---

## Finding

`libvmaf.so.3.0.0` exports **207 internal symbols** that are not part of
the public `vmaf_`-prefixed API. These are visible to every downstream
consumer that `dlopen`s or links against the shared library.

### Reproduction

```bash
nm -D --defined-only build-cpu/src/libvmaf.so.3.0.0 \
  | grep ' [TW] ' | grep -v ' vmaf_' | wc -l
# 207
```

### Categories of leaked symbols

| Category | Count | Examples | Risk |
|---|---|---|---|
| SIMD kernel functions | ~120 | `adm_cm_avx2`, `vif_statistic_8_avx512`, `ssimulacra2_blur_plane_avx2` | Name collision with other image-processing libs |
| libsvm C API | ~20 | `svm_predict`, `svm_train`, `svm_load_model` | **High**: direct collision with any app linking libsvm — symbol interposition will silently redirect calls |
| libsvm C++ internals (mangled) | ~27 | `_ZN5Cache*`, `_ZN6Kernel*`, `_ZN6Solver*` | Name collision with any other libsvm-linked library |
| pdjson JSON parser | ~20 | `json_open_buffer`, `json_next`, `json_get_string` | Collision with other JSON parsers |
| Internal helpers | ~15 | `aligned_malloc`, `aligned_free`, `mkdirp`, `picture_copy`, `_cmp_float` | `aligned_malloc`/`aligned_free` collide with Windows CRT names; `mkdirp` collides with POSIX utilities |
| Feature extractor API | ~3 | `feature_extractor_vector_append`, `feature_extractor_vector_destroy` | Leaks internal lifecycle API |
| IQA helpers | ~5 | `iqa_ssim`, `iqa_convolve`, `iqa_decimate` | Internal IQA shim |
| Compute SSIM/MS-SSIM | ~3 | `compute_ssim`, `compute_ms_ssim`, `ms_ssim_decimate` | Generic name collision |

### Root cause

The meson build compiles all library TUs without `-fvisibility=hidden` and
links without a symbol version script (`--version-script`). The linker flag
`-Wl,--exclude-libs,ALL` only hides symbols that originate from statically
linked *archives* on the link line. The SIMD and feature TUs are compiled as
static sub-libraries (`libx86_avx2.a`, `liblibvmaf_feature.a`, etc.) and then
extracted via `extract_all_objects()` into the final shared library. Extracted
objects are treated by the linker as first-party objects — `--exclude-libs,ALL`
does not apply to them. pdjson, libsvm, and the internal helpers are in the
same category.

### Severity

**High for libsvm** — any downstream binary that also links against libsvm
(which is a common ML library) will experience symbol interposition: the
dynamic linker resolves `svm_predict` to whichever of the two definitions it
finds first. The result is silent mis-dispatch: the downstream SVM calls
libvmaf's copy of the function (with all its hardcoded limitations and
assumptions), or libvmaf calls the downstream's copy, either potentially
corrupting memory or producing wrong predictions.

**Medium for `aligned_malloc`/`aligned_free`** — on Linux these names are not
in glibc, so collision requires another library also shipping them. On Windows
(MSVC runtime) they are standard names that shadow the CRT's own versions.

**Medium for pdjson** — the `json_*` names are generic enough to collide with
any embedded JSON parser (e.g. `json_open_buffer` vs. libjansson's
`json_load_buffer`).

**Low for SIMD kernels** — the mangled names are specific enough that a
collision would be a bizarre coincidence, but the symbols are still unnecessary
ABI surface that prevents future renaming without a major-version bump.

---

## Required fix (outline)

### Option A — `-fvisibility=hidden` + explicit `VMAF_EXPORT` attributes (recommended)

Add `-fvisibility=hidden` to `vmaf_cflags_common` in
`core/src/meson.build`. Add `__attribute__((visibility("default")))` (macro
`VMAF_EXPORT`) to every `vmaf_*` entry point in the public headers under
`core/include/libvmaf/`. The linker will then only export the annotated
symbols.

**Scope of changes**:

- `core/src/meson.build`: add `-fvisibility=hidden` (~2 lines)
- `core/include/libvmaf/*.h`: add `VMAF_EXPORT` to every public function
  declaration (~60–80 entry points across 8 headers)
- `core/include/libvmaf/libvmaf.h`: define the `VMAF_EXPORT` macro
  (platform-aware: `__declspec(dllexport)` on MSVC, `__attribute__((visibility("default")))` on GCC/Clang)
- Verify: `nm -D --defined-only build/src/libvmaf.so.* | grep ' [TW] ' | grep -v ' vmaf_'` should return empty

**Estimated LOC**: ~100–150 lines (mostly mechanical annotation).

### Option B — GNU symbol version script

Add a `libvmaf.map` version script that enumerates every public symbol under
`LIBVMAF_3.0 { global: vmaf_*; local: *; };`. Pass it via
`-Wl,--version-script,libvmaf.map` in `vmaf_link_args`.

**Pros**: no source annotation required.
**Cons**: the `vmaf_*` glob only matches `vmaf_`-prefixed symbols — any
future deliberate addition of a non-`vmaf_`-prefixed public symbol would need
to be listed explicitly. Also, version scripts introduce `SYMVER` ELF
versioning which can complicate downstream static-link scenarios.

**Estimated LOC**: ~50 lines (the map file + meson wiring).

### Option C — hybrid: version script now, visibility attributes in next major

Ship the version script immediately to stop the bleed (Option B). Add
visibility attributes in the next major version bump where ABI compatibility
is already broken.

---

## Decision recommendation

**Proceed with Option A** for the next PR.  It produces a smaller, cleaner
shared library with no versioning side-effects, and the annotation pass is
mechanical. The public API surface is already defined in 8 headers; the
annotation is a grep-and-prefix exercise.

This fix requires touching every public header and the meson build. It does
NOT change any algorithmic behaviour or numerical output. A CI gate verifying
that `nm -D | grep -v vmaf_` returns empty would prevent future leaks.

---

## References

- `nm -D --defined-only build-cpu/src/libvmaf.so.3.0.0 | grep ' [TW] ' | grep -v ' vmaf_'`
  (round-4 angle 6 audit command)
- GCC manual: `-fvisibility=hidden`
- GNU ld: `--version-script`
- ELF symbol versioning: Drepper, "How To Write Shared Libraries", §2.2
