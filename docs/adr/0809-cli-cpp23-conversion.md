# ADR-0809: C++23 Wave 8 — CLI conversion (`cli_parse.c` → `.cpp`, `vmaf.c` → `.cpp`)

<!-- markdownlint-disable MD013 -- ADR/research body text; pre-existing long lines per ADR-0864 tail -->

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cpp23`, `build`, `cli`, `raii`, `fork-local`

## Context

The CLI entry points `core/tools/cli_parse.c` and `core/tools/vmaf.c` were the last
major `.c` files in the `core/tools/` subtree. Several patterns in these files were
suboptimal for C++:

- `NULL` literals instead of `nullptr` — allows implicit integer-to-pointer conversion
  that C++ catches at compile time.
- C-style casts (`(unsigned)`, `(int)`, `(uint16_t *)`) — bypass type-safety checks.
- String-option dispatch via repeated `strcmp` chains — obscures intent and prevents
  compiler dead-code analysis; `std::string_view` comparisons are idiomatic and
  zero-cost.
- `_Noreturn` (C11 attribute) on `usage()` — C++ uses `[[noreturn]]`.
- The three parallel model-tracking arrays in `main()` (`model`, `model_collection`,
  `model_collection_label`) were heap-allocated and freed manually in the goto-cleanup
  block. A leak was possible if future editing added an early-return path before the
  cleanup block. RAII eliminates this class of defect.
- `spinner.h` defined `const char *spinner[]` with external linkage — safe while only
  included in one TU, but non-idiomatic; `static` gives internal linkage in both C
  and C++.

The goto-cleanup spine in `main()` is a load-bearing invariant per ADR-0141 §2: each
cleanup primitive (fclose / video_input_close / vmaf_close / vmaf_*_state_free /
cli_free) must run in reverse-init order on every exit path, and splitting them into
separate RAII helpers would require threading cleanup-relevant pointers through helper
signatures in ways that obscure the unwind chain. The `ModelArrays` RAII struct replaces
only the three parallel arrays whose cleanup is structurally identical (destroy + free);
the spine itself is retained.

## Decision

We will convert `cli_parse.c → cli_parse.cpp` and `vmaf.c → vmaf.cpp` with the
following conservative C++23 idioms:

1. `nullptr` replaces all `NULL` literals.
2. `static_cast<T>()` replaces all C-style casts.
3. `[[nodiscard]]` on all helpers that return error-coded values.
4. `[[noreturn]]` replaces `_Noreturn` on `usage()`.
5. `std::string_view` for option-string comparisons in `resolve_precision_fmt`,
   `parse_pix_fmt`, `parse_aom_ctc`, `parse_nflx_ctc`, the `--backend` dispatch,
   the `--tiny-device` / `--dnn-ep` validation, the `--tiny-resize` validation, and
   `resolve_tiny_device` in `vmaf.cpp`.
6. `ModelArrays` RAII struct in `vmaf.cpp` owns the three model-tracking arrays;
   its destructor calls `vmaf_model_destroy` / `vmaf_model_collection_destroy` and
   frees the backing store, replacing three manual free/destroy loops in the cleanup
   block.
7. `cli_parse.h` gains `extern "C" { ... }` guards so C callers continue to compile
   and link without modification.
8. `spinner.h` gains `static` on `spinner[]` and `spinner_length` for internal linkage.
9. `meson.build` updated: source list uses `.cpp` filenames; `cpp_args` mirrors `c_args`
   for the `#ifdef HAVE_*` defines; `override_options : ['cpp_std=c++23']` pins the
   standard per the pattern established in ADR-0708.

No functional changes. The `vmaf --help` output and the Netflix golden-pair score
(`76.66783`, places=4 ≥ `76.668`) are byte-for-byte identical to the pre-conversion
binary.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Full RAII refactor of goto-cleanup spine | Eliminates all manual cleanup | Requires threading cleanup-relevant pointers through helper signatures; obscures unwind chain | Rejected — load-bearing invariant per ADR-0141 §2 |
| Use `std::string` instead of `std::string_view` | More familiar | Allocates heap memory for transient comparisons | `std::string_view` is zero-cost and correct here |
| Bump project `cpp_std` globally to C++23 now | Simpler build config | Potentially affects SYCL/CUDA TUs not yet tested at C++23 | Per-target override is safe; global bump deferred per ADR-0708 policy |
| Keep `.c` and use C23 features only | No C++ compiler needed | C23 has no `std::string_view`, no RAII, no `[[nodiscard]]` with the same semantics | Cannot address the type-safety and RAII goals with C alone |

## Consequences

- **Positive**: `ModelArrays` destructor guarantees model/collection teardown on all
  exit paths, including future ones. `[[nodiscard]]` on error-returning helpers makes
  it a compile-time error to silently discard a return value. `std::string_view`
  comparisons in option dispatch are self-documenting and remove all `strcmp` chains
  in the option-validation paths.
- **Negative**: Two more C++ TUs in the tools subtree; requires a C++ compiler on the
  build host (already required by `svm.cpp`, all SYCL TUs, and `metadata_handler.cpp`).
- **Neutral / follow-ups**: The original `.c` files are retained alongside the `.cpp`
  files until the PR merges so bisect can diff them; they will be deleted in the merge
  commit.

## References

- [ADR-0708](0708-vmafx-cpp23-internals-pilot.md) — C++23 internals pilot; `extern "C"` guard recipe.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file lint-clean rule; goto-cleanup load-bearing invariant.
- [ADR-0278](0278-nolint-citation-closeout.md) — NOLINT citation policy.
- req: "Convert `core/tools/cli_parse.c` and `core/tools/vmaf.c` to .cpp. CLI parsing benefits from std::expected/std::string_view; CLI main benefits from RAII. Add extern \"C\" guards in cli_parse.h. Conservative C++23 idioms only (nullptr, static_cast, [[nodiscard]])."
