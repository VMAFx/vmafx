# AGENTS.md — libvmaf/src

Scoped orientation for any coding agent working directly inside `libvmaf/src/`.
Parent scope: [`../AGENTS.md`](../AGENTS.md) (libvmaf) and
[`../../AGENTS.md`](../../AGENTS.md) (root).

## Mandatory safety invariants

The following invariants were established during the 2026-05-16 memory-safety
audit (findings #7, #8, #10). Every PR that touches the affected files — or
adds new code in the same category — must preserve them.

### 1. Every `pthread_*_init` return value must be checked (finding #7)

`pthread_mutex_init`, `pthread_cond_init`, and `pthread_rwlock_init` return
non-zero on `ENOMEM` on some POSIX implementations (embedded, musl-based
systems). Ignoring the return value leaves the pool or lock object in an
undefined state; the next `pthread_mutex_lock` call is undefined behaviour.

Pattern to follow: staged init with teardown of already-initialised
primitives on failure (see `vmaf_thread_pool_create` in
[`thread_pool.c`](thread_pool.c)).

### 2. Every `aligned_malloc` / `malloc` call must NULL-check before use (finding #8)

A missing NULL check after `aligned_malloc` causes a null-pointer dereference
on OOM (ASan-detected). In hot-path functions such as `adm_dwt2_*` in
[`feature/adm_tools.c`](feature/adm_tools.c), the allocation must either be
NULL-checked and the function must return an error, or the buffer must be
pre-allocated in the extractor `init` callback so the per-frame path stays
allocation-free. See Power of 10 rule 3 and CERT MEM30-C.

### 3. Size-computing functions that accept external `unsigned w` / `h` must bound-check first (finding #10)

When `w` is large enough that `(w + ALIGN - 1u)` wraps on `unsigned`
arithmetic, the resulting aligned size is 0. The allocator succeeds, and any
pixel read is OOB. Add an early-exit `if (w == 0 || w > 32768u || ...)
return -EINVAL;` guard at the public entry point before any arithmetic.
Pattern: see `vmaf_picture_alloc` in [`picture.c`](picture.c). CERT INT30-C.

### 4. Capacity bounds checks in `output.c` must use `>=`, not `>` (ADR-0606)

All frame-iteration loops in [`output.c`](output.c) guard per-feature
access with:

```c
if (i >= fc->feature_vector[j]->capacity)  /* ADR-0606: >= not > */
    continue;
```

The allocated score array covers indices `0..capacity-1`. Index `capacity`
is one past the end. Using `>` (strictly greater) allows access at
`i == capacity`, which is a heap buffer overread (UB). Under
`MALLOC_PERTURB_=198` (the macOS CI setting), the poisoned byte at
`score[capacity].written` is `0xC6` (truthy), causing spurious "written"
results and downstream SIGSEGV under Apple Clang's UB optimizations.

If an upstream sync or cherry-pick replaces any of the 7 capacity-check
sites with `>`, revert back to `>=` in the same commit.

### 5. Comma-tracking in JSON writers must use explicit `bool first` flags (ADR-0606)

`json_write_pool_score` and `json_write_frames` in [`output.c`](output.c)
track whether a comma separator is needed via explicit `bool first` /
`bool first_frame` flags. Do not replace these with:

- `j > 1` (pool method enum) — wrong when `j == 1` call is skipped and
  `j == 2` is first, producing a leading comma in the JSON object.
- `i > 0` (frame index) — wrong when frame 0 has no written scores and
  frame 3 is first, producing a leading comma in the JSON array.

### 6. macOS locale pushes must use a duplicated base locale

`thread_locale.c::vmaf_thread_locale_push_c()` must not call
`newlocale(..., "C", NULL)` on POSIX hosts. On macOS, allocator poisoning can
leave Apple libc's freshly allocated internal locale object with poisoned
category pointers before `uselocale()` / `fprintf()` touches it, causing the
writer tests to SIGSEGV only on Darwin. The invariant is:

```c
locale_t base = duplocale(LC_GLOBAL_LOCALE);
state->c_locale = newlocale(LC_NUMERIC_MASK, "C", base);
```

Never pass `LC_GLOBAL_LOCALE` directly as the `newlocale()` base; duplicate it
first, and `freelocale(base)` on `newlocale()` failure. The output writers only
need numeric formatting isolation, so do not widen this back to `LC_ALL_MASK`
without a macOS CI run that covers `test_output`, `test_public_api_score`, and
`test_vmaf_use_tiny_model`.

### 7. `test_output` must not include libvmaf implementation TUs

`libvmaf/test/test_output.c` links against libvmaf and reaches the owned
collector through `libvmaf_priv.h::vmaf_feature_collector_get()`. Do not bring
back `#include "libvmaf.c"` or `#include "output.c"` in that test while it also
links libvmaf: Apple ld64 + LTO has resolved the duplicate external definitions
incorrectly under allocator poisoning, crashing the macOS writer tests.

### 8. Output writers flush before popping the C numeric locale

`output.c` writers call `fflush(outfile)` before
`vmaf_thread_locale_pop(locale_state)`. Keep the stream flush inside the
temporary C numeric locale lifetime. Path-based `vmaf_write_output()` uses
`fdopen()` and may otherwise leave the final flush to `fclose()` after the
locale has been restored/freed; that is the macOS-only SIGSEGV shape for
`test_output` and `test_public_api_score`.

### 9. `metadata_handler.cpp` — C++20 pilot; extern "C" guard must not be removed (ADR-0708)

`core/src/metadata_handler.cpp` (previously `metadata_handler.c`) is the first
C++20 internal implementation TU. `metadata_handler.h` carries `extern "C"`
guards that allow `feature_collector.c` (a plain C file) to include the header
and call the three functions without a link-name-mangling mismatch.

Do not:
- Remove the `extern "C"` guards from `metadata_handler.h`.
- Rename the three public symbols (`vmaf_metadata_init`, `vmaf_metadata_append`,
  `vmaf_metadata_destroy`).
- Move the file back to `.c` — `unique_ptr` and `CallbackListDeleter` require
  a C++ compiler.

When porting an upstream Netflix/vmaf commit that modifies the original
`libvmaf/src/metadata_handler.c`, apply the diff content to
`core/src/metadata_handler.cpp` (C code is valid C++; the `extern "C"` block
in the header stays). Run `make test-netflix-golden` post-port.

### 10. `read_json_model.c` — `n_features` / `feature_cap` invariant (ADR-0887)

Every per-feature walker (`parse_slopes`, `parse_intercepts`,
`parse_feature_opts_dicts`, `parse_feature_names`) must call
`sync_n_features(model, i)` so `model->n_features` is the max-merge of every
walker's per-iteration high-water mark. The contract checked by
`validate_feature_arrays` at end of `parse_model_dict` is:

- For every slot `[0, n_features)`, `feature[i].name` must be non-NULL
  (only `parse_feature_names` populates names).
- `feature_cap >= n_features` is guaranteed by `ensure_feature_capacity`
  inside every walker.

Do not:
- Reintroduce unconditional `model->n_features++` in `parse_feature_names`
  (the prior shape double-counted on fuzzer-mangled JSON with repeated
  `feature_names` keys; ADR-0887 reproducer).
- Add a new per-feature walker without calling `sync_n_features`. Even if
  the walker only touches an existing per-slot field (e.g. a future
  `feature[i].chroma_correction`), `feature_cap` and `n_features` will
  drift without the sync.
- Loosen the `validate_feature_arrays` rejection back to a warning.
  Surfacing the contract violation as `-EINVAL` at parse time is the
  ADR-0887 invariant that prevents the OOB-read shape from re-emerging in
  `vmaf_model_destroy`.

When porting an upstream Netflix/vmaf commit that modifies
`libvmaf/src/read_json_model.c` or `libvmaf/src/model.c::vmaf_model_destroy`,
keep both the `sync_n_features` calls and the `min(feature_cap, n_features)`
bound in destroy; re-apply the fork's hunks on top of any upstream changes.

### 11. Vendored libsvm + IQA test files are observation-only (ADR-0952)

`core/test/test_svm_api.c` and `core/test/test_iqa_helpers.c` were added
to lift coverage of the vendored bodies (`core/src/svm.cpp`,
`core/src/feature/iqa/*.c`) from ≈14% to 74% without modifying any
vendored source. The invariant is symmetric to the ADR-0889 cordon:

- These test files **must not** import any private vendored header,
  call any static-internal helper, or rely on any vendored macro
  beyond the public surface declared in `svm.h` / `convolve.h` /
  `decimate.h` / `math_utils.h` / `ssim_tools.h`.
- The vendored cordon `NOLINTBEGIN/NOLINTEND` in `svm.cpp` and the
  `tdistler.com` copyright headers in the IQA helpers stay
  byte-identical across upstream re-pins.
- The `_round()` / `_cmp_float()` asymmetry tests in
  `test_iqa_helpers.c` double as behavioural documentation. They lock
  the asymmetric "trunc toward zero, add sign when |frac| >= 0.5"
  rounding rule. If a future upstream sync rewrites the helper to
  IEEE-754 round-half-to-even, the test fails *by design* — the
  failure surfaces the unintended numerical change at the rebase
  diff, not at an integration-level SSIM/VMAF anomaly.

When porting an upstream Netflix/vmaf commit that modifies the
vendored libsvm or IQA bodies, the test files do not need to follow
the upstream change; they observe public-API contracts that survive
across versions. A test failure post-port is the signal — investigate
the API drift before relaxing the assertion.
