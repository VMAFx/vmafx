---
paths:
  - docs/principles.md
  - .clang-tidy
invariant: Public entry points carry VMAF_EXPORT; C++23 adheres to safety invariants; NOLINT cites ADR within one line.
area: build-and-toolchains
---
<!-- markdownlint-disable MD013 MD060 -->
# Coding standards, symbol visibility, C++23 safety, and NOLINT citations

- **Symbol visibility: every new public entry point needs `VMAF_EXPORT`**
  (fork-local, [ADR-0379](../../docs/adr/0379-libvmaf-symbol-visibility.md) /
  Research-0092). `core/src/meson.build` compiles all TUs with
  `-fvisibility=hidden`; only symbols annotated with `VMAF_EXPORT`
  (defined in `core/include/libvmaf/macros.h`) appear in
  dynamic symbol table of `libvmaf.so`. When adding new public C
  entry point, apply `VMAF_EXPORT` to its declaration in installed
  public header. Attribute propagates from declaration to
  definition if definition TU includes header, so no annotation
  of definition itself is normally required. Exception: if
  definition TU does *not* include public header (see
  `src/dnn/model_loader.h` → `vmaf_dnn_verify_signature`), apply
  `VMAF_EXPORT` to internal declaration instead. Verify after any
  structural change with:

  ```bash
  nm -D --defined-only build/src/libvmaf.so.3.0.0 | grep ' [TW] ' | grep -v ' vmaf_' | wc -l
  # Must print 0
  ```

  On upstream sync: any new `vmaf_*` entry point added upstream that
  fork's headers re-export needs `VMAF_EXPORT` added in same
  merge commit; missing it will silently hide symbol.

- **C→C++23 conversion safety invariants** (adversarial review 2026-05-28,
  `docs/research/cpp23-wave-adversarial-review-20260528.md`):
  When converting `.c` TU to `.cpp` with `std::string_view` / `std::optional` /
  `std::unique_ptr` idioms, verify all of following before merging:

  1. **`string_view::data()` + C-string functions**: `strtol`, `strtod`, `strtof`,
     `strcmp`, `strlen`, `printf("%s", sv.data())` all require NUL-termination.
     If `string_view` is constructed from C-string literal or full C-string
     argument it is safe; if it could ever be substring slice, copy to `std::string`
     first or add `assert(sv.data()[sv.size()] == '\0')`.

  2. **`strtof` vs `strtod` precision**: returning `float` from `strtof`, assigning
     to `double` silently loses precision. If downstream use is `snprintf("%g", dv)`,
     output will be at `float` precision (~7 sig figs), not `double` (~15). Use
     `strtod` when result variable is `double`.

  3. **`make_unique` / `operator new` vs C-caller `free()`**: if struct is allocated
     by `std::make_unique` (uses `operator new`) but C callers may also call `free()`
     on same pointer (e.g. pre-existing teardown paths), this is UB / heap
     corruption. Document in header that `operator delete` (via `vmaf_ref_close`
     or equivalent) is ONLY valid deallocator; search all C callers for direct
     `free(ptr)` on that type.

  4. **`strlen(x) - N` unsigned underflow**: subtracting integer from `size_t`
     (returned by `strlen`) when `strlen(x) < N` wraps to huge value. Always
     check `strlen(x) >= N` first, or use `(len >= N ? len - N : 0)`.

  5. **Recursion in converted code**: Power of 10 rule 1 (no recursion) applies
     equally to `.cpp` files. `mkdirp` is known violator; future conversions must
     replace recursive path-splitting with iterative approach.

  6. **`[[nodiscard]]` on declarations vs definitions**: placing `[[nodiscard]]` only
     on `.cpp` definition without mirroring it in `extern "C"` declaration in
     header means C++ callers seeing only header will not get diagnostic.
     Always add `[[nodiscard]]` to header declaration (inside `extern "C"` block
     — C compilers silently ignore attribute).

  7. **Isolated C++ static libs carry NO `cpp_std` override (epic #1241 cleanup)**:
     `gpu_dispatch_env_cpp23_lib` (ADR-0858), `metadata_handler_cpp20_lib` (ADR-0708),
     `log_cpp23_lib`, `opt_cpp23_lib`, `picture_pool_cpp23_lib`, `gpu_picture_pool_cpp23_lib`,
     `read_json_model_cpp23_lib`, `libvmaf_cpu_static_lib`, `vmaf` / `vmafx` tools,
     `test_cli_parse*` / `test_picture_pool_cpp_error_paths` tests and `fuzz_cli_parse` are all
     compiled at project-wide C++ standard selected by Meson's built-in
     preference list (ADR-1003 / ADR-1273). Former
     `override_options : ['cpp_std=...']` entries (and
     `libvmaf_cpu_cpp_std` token variable) created conflicting duplicate flags.
     Never re-add per-target `cpp_std` overrides for new
     `.c → .cpp` conversions; do keep isolated-lib + `extract_all_objects` link pattern
     (it is what test targets consume). Only `override_options` remaining are
     `b_lto=false` ones (AVX-512 symbol visibility, macOS `test_output`) and those are real.

- **Three cited `cppcheck-suppress constParameterPointer` markers** are
  deliberate, not debt: `vmaf_context_get_backend` (public ABI prototype in
  `include/libvmaf/libvmaf.h` is frozen), `read_pictures_validate_and_prep`
  (`vmaf_sycl_shared_frame_upload()` takes mutable pictures on SYCL
  build cppcheck never analyses) and `vmaf_feature_collector_unmount_model`
  (public C declaration fixes mutable model-pointer signature in
  `feature/feature_collector.cpp`). Drop marker only when its cited constraint
  is gone. `vmaf_feature_collector_get()` (`libvmaf_priv.h`) takes
  `const VmafContext *` — keep declaration and definition in step.
- **C translation units keep `NULL`** (ADR-1138): `libvmaf.c` and `predict.c`
  carry file-scoped
  `NOLINTBEGIN/END(modernize-use-nullptr)` bracket. Never rewrite `NULL` to
  `nullptr` in C sources (MSVC `/std:clatest` does not document it; upstream
  parity), keep `NOLINTEND` line at end of file when appending code.

## Every `NOLINT` names its ADR, and the citation has to be within one line

[ADR-0141](../../docs/adr/0141-touched-file-cleanup-rule.md) §2 requires each
suppression to cite load-bearing invariant that forces it, in format
[ADR-0278](../../docs/adr/0278-t7-5-nolint-sweep.md) fixed. Since CPU-lane
sweep (epic #1237) `core/src`, `core/tools` and `core/test` are at **zero**
uncited markers, and `scripts/ci/tidy-ratchet.py` measures that.

Three mechanics bite when adding or moving suppression:

- **Window is ±1 line.** `tidy-ratchet.py::count_uncited_nolints` accepts
  `ADR-NNNN` on previous line, same line, next line, or anywhere
  inside `/* … */` block comment that *holds* marker. ADR named three
  lines above in separate comment block does **not** count — that is exactly
  how nine survivors of first sweep pass slipped through.
- **long trailing comment can move marker off its diagnostic.** Appending
  citation to `free(p); // NOLINT(...)` can push line past 100-column
  budget. clang-format then wraps *code*, leaving `// NOLINT` on
  continuation line while clang-tidy still reports finding at `free`
  token — silently dead suppression. When line no longer fits, convert to
  preceding `// NOLINTNEXTLINE(...) — ADR-NNNN` instead of letting
  clang-format re-wrap. `core/src/ref.cpp`, `core/src/opt.cpp` and
  `core/src/dnn/model_loader.c` are in that form for this reason.
- **`NOLINTNEXTLINE` justification must never wrap onto second comment
  line.** Directive applies to line immediately following it. So
  `// NOLINTNEXTLINE(readability-function-size) — ADR-0141 §2 /` followed by
  `// ADR-0159 …: reason` points suppression at *comment*, not at
  function — diagnostic comes back. Ratchet still counts marker
  as cited, so citation gate stays green while warning count regresses.
  Only thing that catches it is `clang-tidy -p build <file>` against
  merge base. Keep directive on one line (short `— ADR-NNNN` suffix fits
  inside 100 columns), put prose in block comment above it;
  `core/src/feature/{x86,arm64}/psnr_hvs_*.c` and
  `core/src/feature/{x86,arm64}/ssimulacra2_host_*.c` are in that form for this
  reason.
- **Word "NOLINT" in prose is counted as marker.** `NOLINT_RE` matches
  bare token, so comment saying "NOLINT justification: …" registers as
  uncited suppression even when no directive exists. Write "suppression
  justification" instead.

Cite ADR that governs site: upstream-parity → ADR-0141 §2;
SIMD bit-exactness → ADR-0138 / ADR-0139 / ADR-0159 / ADR-0161 / ADR-0252;
public-ABI `const_cast` in C++23 pilot → ADR-0721.
