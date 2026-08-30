<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src

Scoped orientation for any coding agent working directly inside `core/src/`.
Parent scope: [`../AGENTS.md`](../AGENTS.md) (core) and
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

### 2. Every `aligned_malloc` / `malloc` must NULL-check before use (#8)

A missing NULL check after `aligned_malloc` causes a null-pointer dereference
on OOM (ASan-detected). In hot-path functions such as `adm_dwt2_*` in
[`feature/adm_tools.c`](feature/adm_tools.c), the allocation must either be
NULL-checked and the function must return an error, or the buffer must be
pre-allocated in the extractor `init` callback so the per-frame path stays
allocation-free. See Power of 10 rule 3 and CERT MEM30-C.

### 3. Size-computing functions must bound-check `w`/`h` first (#10)

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

### 5. JSON writers must use explicit `bool first` flags (ADR-0606)

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

`core/test/test_output.c` links against libvmaf and reaches the owned
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

### 9. `metadata_handler.cpp` — C++20 pilot; keep `extern "C"` (ADR-0708)

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
- Drop the `free(model->feature[index].name)` that precedes the `strdup`
  in `append_feature_name` (both `read_json_model.c` and the C++23 twin
  `read_json_model.cpp`). A duplicate `feature_names` key re-runs
  `parse_feature_names` from index 0 and overwrites `feature[index].name`;
  without the free, the prior strdup'd name is orphaned. `vmaf_model_destroy`
  walks only the current slot occupants, so the orphan is unreachable and
  leaks on both the validation-error and success paths (nightly
  `fuzz_json_model` LeakSanitizer lane, `Direct leak of N byte(s)`). Keep the
  two parser variants in lockstep — the leak is identical in both.

When porting an upstream Netflix/vmaf commit that modifies
`libvmaf/src/read_json_model.c` or `libvmaf/src/model.c::vmaf_model_destroy`,
keep both the `sync_n_features` calls and the `min(feature_cap, n_features)`
bound in destroy, plus the `free`-before-`strdup` guard in
`append_feature_name`; re-apply the fork's hunks on top of any upstream
changes.

### 11. Vendored libsvm + IQA test files are observation-only (ADR-0952)

`core/test/test_svm_api.c`, `core/test/test_svm_multiclass.c`, and
`core/test/test_iqa_helpers.c` were added to lift coverage of the vendored
bodies (`core/src/svm.cpp`, `core/src/feature/iqa/*.c`) without modifying
any vendored source. The invariant is symmetric to the ADR-0889 cordon:

`test_svm_multiclass.c` specifically exercises the sequential-realloc
double-free path fixed in PR #708 — 17-class and 32-class C_SVC fixtures
force the `max_nr_class=16→32` realloc doubling in `svm_group_classes()`,
and a 17-class NU_SVC fixture triggers the same path in
`svm_check_parameter()`. Under ASan/UBSan any regression to the double-free
pattern aborts immediately. (ADR-1066)

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

### 10. `.cpp` files lint-clean to `modernize-*` profile (ADR-0915)

`.clang-tidy` enables the full `modernize-*` family minus four explicit
opt-outs (`-modernize-use-trailing-return-type`, `-modernize-use-auto`,
`-modernize-avoid-c-arrays`, `-modernize-use-nodiscard`). The CPU-built
`.cpp` translation units (`core/src/cpu.cpp`,
`core/src/feature/feature_collector.cpp`,
`core/src/metadata_handler.cpp`) are lint-clean to this profile.

When porting an upstream Netflix `.c` patch onto one of these `.cpp`
files: prefer `nullptr` over `NULL`, prefer `<cstdlib>`/`<cstring>` over
`<stdlib.h>`/`<string.h>`, drop `<stdbool.h>` includes (in C++ `bool` is
a keyword), and use `auto*` for `static_cast<T*>(malloc(...))`-style
initialisers where the cast already spells the type. These match the
checks enabled by ADR-0915; deviating reintroduces warnings that the
touched-file rule (ADR-0141) requires you to discharge in the same PR.

### 10. Vendored libsvm — three fork patches must not regress on sync (ADR-0889)

`core/src/svm.cpp` + `core/src/svm.h` are a verbatim vendored copy of
upstream libsvm 3.24 (Chih-Chung Chang / Chih-Jen Lin), wrapped in a
file-level `NOLINTBEGIN` / `NOLINTEND` cordon so the fork's
touched-file lint-clean rule does not re-flow the vendored body. Three
fork-local patch families live inside that cordon and must survive any
future upstream sync:

1. **Thread-locale isolation** — both `SVMModelParserFileSource` and
   `SVMModelParserBufferSource` constructors call
   `buffer.imbue(std::locale::classic())`. Removing this re-introduces
   ADR-0137's locale-perturbation hazard on hosts whose `LC_NUMERIC`
   uses `,` as the decimal separator. *Cite ADR-0137 in any commit
   touching these lines.*

2. **JSON in-memory entry point** — `svm_parse_model_from_buffer` (and
   the `SVMModelParserBufferSource` class that backs it) is fork-added;
   upstream libsvm has only `svm_load_model(const char *path)`. The
   fork's `read_json_model.c` depends on the buffer entry point.
   Removing it breaks JSON-embedded model loading.

3. **SAN-MODEL-MALLOC-OOB hardening** — every `Malloc(...)` call in
   `parse_header()` and `parse_support_vectors()` whose size depends on
   `nr_class` or `total_sv` is gated by `exceptAssert(... > 0 && ... <=
   VMAF_SVM_MAX_AXIS_COUNT, ...)`. The bound `VMAF_SVM_MAX_AXIS_COUNT
   (1 << 24)` is fork-defined. The `sv_buffer.empty()` post-parse guard
   is fork-added. The `model->nr_class > 0` row-ordering precondition
   on `rho`, `label`, `probA`, `probB`, `nr_sv` is fork-added.
   Regression coverage lives in `core/test/test_svm_parser.c` (suite
   `fast`). *Cite the sanitizer-real-bug-fixes changelog and ADR-0889
   in any commit touching these guards.*

Additionally:

- `model->free_sv = 1;` at the end of `parse_support_vectors` is the
  load-bearing invariant for `svm_free_and_destroy_model`'s ownership
  transfer. Vendor-original; do not flip it.
- `LIBSVM_VERSION 324` in `svm.h` is the pin. A sync to a newer
  version must re-apply the three patch families above and re-run
  `core/test/test_svm_parser.c` + `core/test/test_predict` +
  `core/test/test_model`. See ADR-0889 for the deferral rationale on
  the upstream 3.36 sync.

### 10. Fork diagnostics route through `vmaf_log`, not `fprintf(stderr)`

libvmaf exposes a user-installable log callback via
`vmaf_set_log_callback` and a level filter via `vmaf_set_log_level`.
Direct `fprintf(stderr, ...)` / `printf(...)` bypasses both surfaces —
the message reaches the terminal regardless of the user's installed
callback or chosen verbosity, and embedded callers (FFmpeg filter, MCP
server, future bindings) can never capture it.

For any new fork-local diagnostic (errors, warnings, debug traces),
use `vmaf_log(VMAF_LOG_LEVEL_{ERROR,WARNING,INFO,DEBUG}, fmt, ...)`
declared in [`log.h`](log.h). C++ TUs include the header inside an
`extern "C" { }` block — see `core/src/sycl/common.cpp` and
`core/src/sycl/dispatch_strategy.cpp` for the pattern.

Exceptions — direct stream writes are correct in these cases:

- `core/src/log.{c,cpp}` — the log implementation itself.
- CLI tools under `core/tools/` — stdout score / JSON output is the
  contract.
- Pull-style "print on request" SYCL APIs
  (`vmaf_sycl_list_devices`, `vmaf_sycl_print_timing`,
  `vmaf_sycl_profiling_print`) — the stream IS the function's contract;
  routing through the callback would silently drop output for callers
  without an installed callback at the matching level.
- Vendored libsvm (`core/src/svm.cpp`) and upstream-mirror feature
  extractors (`feature/vif.c`, `feature/adm.c`, etc.) — leave as-is to
  preserve upstream-sync semantics; route only if the touching PR has
  an upstream-sync impact note.

See `docs/research/logging-consistency-audit-2026-05-30.md` for the
audit that established this invariant.

### Out-parameter init functions must clear the handle on every failure

Functions with the shape `int X_init(X **out, ...)` that publish the
allocation via the caller's `*out` must guarantee `*out == NULL` on any
non-success return — including failure paths that take an internal `goto`
and free the object before returning. The trap is the combined-assignment
idiom `X *const p = *out = malloc(...);` which publishes the pointer to
the caller *before* later `goto free_*` paths free it.

If the caller stores the handle in a long-lived context (e.g.
`VmafContext.cuda.ring_buffer`), the natural teardown (`vmaf_close()` →
`X_close(*out)`) will then UAF on the freed object. The fix is mechanical:
set `*out = NULL` after every `free()` in the failure-cleanup chain (and
explicitly on the early-malloc-failure path even though the assignment
already stored NULL there). The contract this pins is: "caller may inspect
`*out` only on success; a non-zero return guarantees `*out == NULL`."

Pattern: see `vmaf_gpu_picture_pool_init` in
[`gpu_picture_pool.c`](gpu_picture_pool.c). Regression test:
`core/test/test_gpu_picture_pool_uaf.c`.

### PREV_REF batch dispatch: unref before memset, zero f->prev_ref (ADR-1072)

`threaded_extract_batch_func` in `libvmaf.c` feeds PREV_REF extractors by
copying `f->prev_ref` into `fex->prev_ref` via a bare struct copy (no
`vmaf_picture_ref` — the VmafRef* is shared, not reference-counted
separately).  After `vmaf_feature_extractor_context_extract()`:

- **SUCCESS**: the PREV_REF SWAP in `feature_extractor.cpp` has decremented
  the old-frame VmafRef (via the struct-copy alias) and bumped the current
  frame into `fex->prev_ref` with an extra refcount.
- **ERROR**: `fex->prev_ref` is unchanged (still the struct-copy alias).

**In both cases**: call `vmaf_picture_unref(&fex->prev_ref)` before
`memset(&fex->prev_ref, 0, ...)` to release that counted reference.  Then
call `memset(&f->prev_ref, 0, ...)` to prevent the `unref:` block at the
bottom of the function from double-freeing the now-consumed VmafRef.

A bare `memset` without the prior unref leaks one picture-pool slot per
PREV_REF frame, exhausting the pool and deadlocking
`vmaf_picture_pool_fetch` in `pthread_cond_wait` after ~pool_size frames.

The serial path (`read_pictures_dispatch_one`) uses `vmaf_picture_ref` for
the copy (ADR-0778) and already calls `vmaf_picture_unref` before the memset;
these two paths must stay consistent.

**Rebase-sensitive**: any branch that re-opens or modifies the
`VMAF_FEATURE_EXTRACTOR_PREV_REF` block in `threaded_extract_batch_func`
must preserve both the unref-before-memset and the zero-f->prev_ref.

### framesync producer-error paths must call vmaf_framesync_abort (ADR-1092)

`retrieve_filled_data` waits in `pthread_cond_wait` until a matching
BUF_FILLED entry appears.  If the producer thread exits without calling
`submit_filled_data` for an index the consumer is waiting on, the consumer
hangs forever.

Any code path that acquires a framesync buffer (via
`vmaf_framesync_acquire_new_buf`) and then returns an error before calling
`vmaf_framesync_submit_filled_data` for that index **must** call
`vmaf_framesync_abort(fs_ctx)` first.  This sets the `aborted` flag and
broadcasts on the condvar, causing all blocked `retrieve_filled_data` callers
to return `-ECANCELED`.

`vmaf_framesync_destroy` calls `vmaf_framesync_abort` as a safety net, but
relying on that alone delays the wake-up until destroy time, which is after
`vmaf_thread_pool_wait` — meaning the thread pool wait would still hang.

**Rebase-sensitive**: any branch adding new framesync producer paths (feature
extractors, GPU dispatch loops) must include a `vmaf_framesync_abort` call on
all error exits from `extract()` or equivalent before returning.

## Doxygen comment invariant (ADR-1096)

The following `core/src/*.h` internal headers now carry Doxygen `@brief`,
`@param`, and `@return` annotations: `framesync.h`, `thread_pool.h`,
`picture_pool.h`, `predict.h`, `fex_ctx_vector.h`, `ref.h`, `mem.h`,
`log.h`, `opt.h`, `dict.h`.

**Invariant for rebases and follow-up branches**: when adding, renaming, or
removing function signatures in these headers, update the corresponding
Doxygen block in the same commit. A dangling `@param` for a deleted argument
or a missing `@param` for a new one is a docs regression. Run
`doxygen Doxyfile 2>&1 | grep warning` to check — zero new warnings is the
bar.
