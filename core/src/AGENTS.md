<!-- markdownlint-disable MD013 -->
# AGENTS.md — core/src

Scoped orientation for coding agent working directly inside `core/src/`.
Parent scope: [`../AGENTS.md`](../AGENTS.md) (core) and
[`../../AGENTS.md`](../../AGENTS.md) (root).

## Windows CUDA compiler discovery

`meson.build` must assign `cl_path` on both `vswhere` and `PATH` discovery
routes. NVCC's `-ccbin` and MSVC include discovery consume that same path.
Keep configure regression in `../test/test_windows_cuda_compiler_discovery.py`
when rebasing Windows discovery block from Netflix PR #1472.

## C++ targets take `vmaf_cppflags_common` (ADR-0379)

- every C++ target linked into libvmaf passes `cpp_args : vmaf_cppflags_common` (`core/src/meson.build`). No per-target define lists.
- missing -> no `-fvisibility=hidden` -> internal symbols exported from `libvmaf.so` (72 did, until 2026-09-18); also no `HAVE_CUDA` / `HAVE_SYCL` -> `VmafPicturePrivate` layout skew (PR #840).
- `vmaf_cppflags_common` derived after last `vmaf_cflags_common +=`; new defines go before that line.
- gate: `meson test -C build check_exported_symbols` (`core/test/check_exported_symbols.py`).

## Mandatory safety invariants

`pdjson.c` enforces ADR-1061 limit as **512 containers**, not zero-based
maximum stack index. `push()` checks next depth and completes allocation
before publishing `stack_top`; depth or allocation failure must leave the
accepted stack intact. Reject zero or oversized `PDJSON_STACK_INC` overrides
before invoking allocator; retain separately compiled growth tests.
Preserve first-error diagnostic, streaming/peek/reset
contract, UTF-8 validation and private getter const qualifiers. Dedicated
`core/test/test_pdjson.c` suite covers these contracts. Never restore the old
blanket NOLINT; only file-wide exception is ADR-1138's C `NULL` compatibility.

Following invariants established during 2026-05-16 memory-safety
audit (findings #7, #8, #10). Every PR that touches affected files — or
adds new code in same category — must preserve them.

### 1. Every `pthread_*_init` return value must be checked (finding #7)

`pthread_mutex_init`, `pthread_cond_init`, and `pthread_rwlock_init` return
non-zero on `ENOMEM` on some POSIX implementations (embedded, musl-based
systems). Ignoring return value leaves pool or lock object in
undefined state; next `pthread_mutex_lock` call = undefined behaviour.

Pattern to follow: staged init with teardown of already-initialised
primitives on failure (see `vmaf_thread_pool_create` in
[`thread_pool.c`](thread_pool.c)).

`vmaf_framesync_init` follows the same staged contract for both mutexes and its
condition variable. It sets the caller's output to `NULL` before allocation,
publishes the context only after every primitive and the first queue node are
ready, returns the negated pthread error, and destroys only primitives whose
initializers succeeded. Do not move `*fs_ctx = ctx` back above those stages or
collapse the ordered unwind labels.

### 2. Every `aligned_malloc` / `malloc` must NULL-check before use (#8)

Missing NULL check after `aligned_malloc` causes null-pointer dereference
on OOM (ASan-detected). In hot-path functions such as `adm_dwt2_*` in
[`feature/adm_tools.c`](feature/adm_tools.c): allocation must be
NULL-checked with function returning error, or buffer must be
pre-allocated in extractor `init` callback so per-frame path stays
allocation-free. See Power of 10 rule 3 and CERT MEM30-C.

### 3. Size-computing functions must bound-check `w`/`h` first (#10)

When `w` is large enough that `(w + ALIGN - 1u)` wraps on `unsigned`
arithmetic, resulting aligned size = 0. Allocator succeeds, and any
pixel read is OOB. Add an early-exit `if (w == 0 || w > 32768u || ...)
return -EINVAL;` guard at public entry point before any arithmetic.
Pattern: see `vmaf_picture_alloc` in [`picture.c`](picture.c). CERT INT30-C.

### 4. Capacity bounds checks in `output.c` must use `>=`, not `>` (ADR-0606)

All frame-iteration loops in [`output.c`](output.c) guard per-feature
access with:

```c
if (i >= fc->feature_vector[j]->capacity)  /* ADR-0606: >= not > */
    continue;
```

Allocated score array covers indices `0..capacity-1`. Index `capacity`
= one past end. Using `>` (strictly greater) allows access at
`i == capacity`, = heap buffer overread (UB). Under
`MALLOC_PERTURB_=198` (macOS CI setting), poisoned byte at
`score[capacity].written` = `0xC6` (truthy), causing spurious "written"
results and downstream SIGSEGV under Apple Clang's UB optimizations.

If upstream sync or cherry-pick replaces any of 7 capacity-check
sites with `>`, revert back to `>=` in same commit.

### 5. JSON writers must use explicit `bool first` flags (ADR-0606)

`json_write_pool_score` and `json_write_frames` in [`output.c`](output.c)
track whether comma separator needed via explicit `bool first` /
`bool first_frame` flags. Never replace these with:

- `j > 1` (pool method enum) — wrong when `j == 1` call skipped and
  `j == 2` first, producing leading comma in JSON object.
- `i > 0` (frame index) — wrong when frame 0 has no written scores and
  frame 3 first, producing leading comma in JSON array.

### 6. macOS locale pushes must use a duplicated base locale

`thread_locale.c::vmaf_thread_locale_push_c()` must not call
`newlocale(..., "C", NULL)` on POSIX hosts. On macOS, allocator poisoning can
leave Apple libc's freshly allocated internal locale object with poisoned
category pointers before `uselocale()` / `fprintf()` touches it, causing
writer tests to SIGSEGV only on Darwin. Invariant:

```c
locale_t base = duplocale(LC_GLOBAL_LOCALE);
state->c_locale = newlocale(LC_NUMERIC_MASK, "C", base);
```

Never pass `LC_GLOBAL_LOCALE` directly as `newlocale()` base; duplicate it
first, and `freelocale(base)` on `newlocale()` failure. Output writers only
need numeric formatting isolation, so never widen this back to `LC_ALL_MASK`
without macOS CI run covering `test_output`, `test_public_api_score`, and
`test_vmaf_use_tiny_model`.

### 7. `test_output` must not include libvmaf implementation TUs

`core/test/test_output.c` links against libvmaf and reaches owned
collector through `libvmaf_priv.h::vmaf_feature_collector_get()`. Never bring
back `#include "libvmaf.c"` or `#include "output.c"` in that test while it also
links libvmaf: Apple ld64 + LTO has resolved duplicate external definitions
incorrectly under allocator poisoning, crashing macOS writer tests.

### 8. Output writers flush before popping the C numeric locale

`output.c` writers call `fflush(outfile)` before
`vmaf_thread_locale_pop(locale_state)`. Keep stream flush inside
temporary C numeric locale lifetime. Path-based `vmaf_write_output()` uses
`fdopen()` and may otherwise leave final flush to `fclose()` after
locale has been restored/freed; that = macOS-only SIGSEGV shape for
`test_output` and `test_public_api_score`.

### 9. `metadata_handler.cpp` — C++20 pilot; keep `extern "C"` (ADR-0708)

`core/src/metadata_handler.cpp` (previously `metadata_handler.c`) = first
C++20 internal implementation TU. `metadata_handler.h` carries `extern "C"`
guards that allow `feature_collector.c` (plain C file) to include header
and call three functions without link-name-mangling mismatch.

Never:

- Remove `extern "C"` guards from `metadata_handler.h`.
- Rename three public symbols (`vmaf_metadata_init`, `vmaf_metadata_append`,
  `vmaf_metadata_destroy`).
- Move file back to `.c` — `unique_ptr` and `CallbackListDeleter` require
  C++ compiler.

When porting upstream Netflix/vmaf commit modifying original
`core/src/metadata_handler.cpp`: apply diff content to
`core/src/metadata_handler.cpp` (C code valid C++; `extern "C"` block
in header stays). Run `make test-netflix-golden` post-port.

### 10. `read_json_model.c` — `n_features` / `feature_cap` invariant (ADR-0887)

Every per-feature walker (`parse_slopes`, `parse_intercepts`,
`parse_feature_opts_dicts`, `parse_feature_names`) must call
`sync_n_features(model, i)` so `model->n_features` = max-merge of every
walker's per-iteration high-water mark. Contract checked by
`validate_feature_arrays` at end of `parse_model_dict`:

- For every slot `[0, n_features)`, `feature[i].name` must be non-NULL
  (only `parse_feature_names` populates names).
- `feature_cap >= n_features` guaranteed by `ensure_feature_capacity`
  inside every walker.

Never:

- Reintroduce unconditional `model->n_features++` in `parse_feature_names`
  (prior shape double-counted on fuzzer-mangled JSON with repeated
  `feature_names` keys; ADR-0887 reproducer).
- Add new per-feature walker without calling `sync_n_features`. Even if
  walker only touches an existing per-slot field (e.g. future
  `feature[i].chroma_correction`), `feature_cap` and `n_features` drift
  without the sync.
- Loosen `validate_feature_arrays` rejection back to a warning.
  Surfacing contract violation as `-EINVAL` at parse time = the
  ADR-0887 invariant preventing OOB-read shape from re-emerging in
  `vmaf_model_destroy`.
- Drop `free(model->feature[index].name)` that precedes `strdup`
  in `append_feature_name` (both `read_json_model.c` and C++23 twin
  `read_json_model.cpp`). Duplicate `feature_names` key re-runs
  `parse_feature_names` from index 0 and overwrites `feature[index].name`;
  without the free, prior strdup'd name is orphaned. `vmaf_model_destroy`
  walks only current slot occupants, so orphan is unreachable and
  leaks on both validation-error and success paths (nightly
  `fuzz_json_model` LeakSanitizer lane, `Direct leak of N byte(s)`). Keep
  two parser variants in lockstep — leak is identical in both.

When porting upstream Netflix/vmaf commit modifying
`core/src/read_json_model.c` or `core/src/model.c::vmaf_model_destroy`,
keep both `sync_n_features` calls and `min(feature_cap, n_features)`
bound in destroy, plus `free`-before-`strdup` guard in
`append_feature_name`; re-apply fork's hunks on top of any upstream
changes.

### 11. Vendored libsvm + IQA test files are observation-only (ADR-0952)

`core/test/test_svm_api.c`, `core/test/test_svm_multiclass.c`, and
`core/test/test_iqa_helpers.c` were added to lift coverage of vendored
bodies (`core/src/svm.cpp`, `core/src/feature/iqa/*.c`) without modifying
any vendored source. Invariant symmetric to ADR-0889 cordon:

`test_svm_multiclass.c` specifically exercises sequential-realloc
double-free path fixed in PR #708 — 17-class and 32-class C_SVC fixtures
force `max_nr_class=16→32` realloc doubling in `svm_group_classes()`,
and 17-class NU_SVC fixture triggers same path in
`svm_check_parameter()`. Under ASan/UBSan any regression to double-free
pattern aborts immediately. (ADR-1066)

- These test files **must not** import any private vendored header,
  call any static-internal helper, or rely on any vendored macro
  beyond public surface declared in `svm.h` / `convolve.h` /
  `decimate.h` / `math_utils.h` / `ssim_tools.h`.
- Vendored cordon `NOLINTBEGIN/NOLINTEND` in `svm.cpp` and
  `tdistler.com` copyright headers in IQA helpers stay
  byte-identical across upstream re-pins.
- `_round()` / `_cmp_float()` asymmetry tests in
  `test_iqa_helpers.c` double as behavioural documentation. They lock
  the asymmetric "trunc toward zero, add sign when |frac| >= 0.5"
  rounding rule. If future upstream sync rewrites helper to
  IEEE-754 round-half-to-even, test fails *by design* — failure
  surfaces unintended numerical change at rebase diff, not at an
  integration-level SSIM/VMAF anomaly.

When porting upstream Netflix/vmaf commit modifying
vendored libsvm or IQA bodies, test files do not need to follow
upstream change; they observe public-API contracts that survive
across versions. Test failure post-port is the signal — investigate
API drift before relaxing assertion.

### 10. `.cpp` files lint-clean to `modernize-*` profile (ADR-0915)

`.clang-tidy` enables full `modernize-*` family minus four explicit
opt-outs (`-modernize-use-trailing-return-type`, `-modernize-use-auto`,
`-modernize-avoid-c-arrays`, `-modernize-use-nodiscard`). CPU-built
`.cpp` translation units (`core/src/cpu.cpp`,
`core/src/feature/feature_collector.cpp`,
`core/src/metadata_handler.cpp`) are lint-clean to this profile.

When porting upstream Netflix `.c` patch onto one of these `.cpp`
files: prefer `nullptr` over `NULL`, prefer `<cstdlib>`/`<cstring>` over
`<stdlib.h>`/`<string.h>`, drop `<stdbool.h>` includes (in C++ `bool` is
a keyword), and use `auto*` for `static_cast<T*>(malloc(...))`-style
initialisers where cast already spells type. These match checks
enabled by ADR-0915; deviating reintroduces warnings that touched-file
rule (ADR-0141) requires discharging in same PR.

### 10. Vendored libsvm — four fork patches must not regress on sync (ADR-0889, Research-2094)

`core/src/svm.cpp` + `core/src/svm.h` = verbatim vendored copy of
upstream libsvm 3.24 (Chih-Chung Chang / Chih-Jen Lin), wrapped in
file-level `NOLINTBEGIN` / `NOLINTEND` cordon so fork's
touched-file lint-clean rule does not re-flow vendored body. Four
fork-local patch families live inside that cordon and must survive any
future upstream sync:

1. **Thread-locale isolation** — both `SVMModelParserFileSource` and
   `SVMModelParserBufferSource` constructors call
   `buffer.imbue(std::locale::classic())`. Removing this re-introduces
   ADR-0137's locale-perturbation hazard on hosts whose `LC_NUMERIC`
   uses `,` as decimal separator. *Cite ADR-0137 in any commit
   touching these lines.*

2. **JSON in-memory entry point** — `svm_parse_model_from_buffer` (and
   `SVMModelParserBufferSource` class that backs it) is fork-added;
   upstream libsvm has only `svm_load_model(const char *path)`. Fork's
   `read_json_model.c` depends on buffer entry point.
   Removing it breaks JSON-embedded model loading.

3. **SAN-MODEL-MALLOC-OOB hardening** — every `Malloc(...)` call in
   `parse_header()` and `parse_support_vectors()` whose size depends on
   `nr_class` or `total_sv` is gated by `exceptAssert(... > 0 && ... <=
   VMAF_SVM_MAX_AXIS_COUNT, ...)`. The bound `VMAF_SVM_MAX_AXIS_COUNT
   (1 << 24)` is fork-defined. The `sv_buffer.empty()` post-parse guard
   is fork-added. `model->nr_class > 0` row-ordering precondition
   on `rho`, `label`, `probA`, `probB`, `nr_sv` is fork-added.
   Regression coverage lives in `core/test/test_svm_parser.c` (suite
   `fast`). *Cite sanitizer-real-bug-fixes changelog and ADR-0889
   in any commit touching these guards.*

4. **Solver RAII lifecycle and loop safety (Research-2094)** — `Solver`
   and `Solver_NU` manage working heap arrays (`p`, `y`, `alpha`,
   `alpha_status`, `active_set`, `G`, `G_bar`) via idempotent
   `solve_cleanup()` invoked by `solve_finish()`, `~Solver()`, and entry
   of `solve_setup()`. Deleted copy/assignment operations prevent shallow
   copying and double-free. In `parse_support_vectors()`, support-vector
   parsing replaces outer `for` loop counter mutation with bounded `while`
   loop verifying sentinel termination. Eliminates CodeQL alerts 1222–1226.

Additionally:

- `model->free_sv = 1;` at end of `parse_support_vectors` is the
  load-bearing invariant for `svm_free_and_destroy_model`'s ownership
  transfer. Vendor-original; never flip it.
- `LIBSVM_VERSION 324` in `svm.h` = the pin. A sync to newer
  version must re-apply three patch families above and re-run
  `core/test/test_svm_parser.c` + `core/test/test_predict` +
  `core/test/test_model`. See ADR-0889 for deferral rationale on
  upstream 3.36 sync.

### 10. Fork diagnostics route through `vmaf_log`, not `fprintf(stderr)`

libvmaf exposes user-installable log callback via
`vmaf_set_log_callback` and level filter via `vmaf_set_log_level`.
Direct `fprintf(stderr, ...)` / `printf(...)` bypasses both surfaces —
message reaches terminal regardless of user's installed
callback or chosen verbosity, and embedded callers (FFmpeg filter, MCP
server, future bindings) can never capture it.

For any new fork-local diagnostic (errors, warnings, debug traces),
use `vmaf_log(VMAF_LOG_LEVEL_{ERROR,WARNING,INFO,DEBUG}, fmt, ...)`
declared in [`log.h`](log.h). C++ TUs include header inside an
`extern "C" { }` block — see `core/src/sycl/common.cpp` and
`core/src/sycl/dispatch_strategy.cpp` for pattern.

Exceptions — direct stream writes are correct in these cases:

- `core/src/log.{c,cpp}` — log implementation itself.
- CLI tools under `core/tools/` — stdout score / JSON output is the
  contract.
- Pull-style "print on request" SYCL APIs
  (`vmaf_sycl_list_devices`, `vmaf_sycl_print_timing`,
  `vmaf_sycl_profiling_print`) — stream IS function's contract;
  routing through callback would silently drop output for callers
  without an installed callback at matching level.
- Vendored libsvm (`core/src/svm.cpp`) and upstream-mirror feature
  extractors (`feature/vif.c`, `feature/adm.c`, etc.) — leave as-is to
  preserve upstream-sync semantics; route only if touching PR has
  an upstream-sync impact note.

`log.c` has one additional C23 toolchain invariant: Clang lowers `va_start`
to `__builtin_c23_va_start`, which Clang 21's VA-list analyzer does not model.
Keep the Clang+C23 branch on the semantically identical
`__builtin_va_start(args, fmt)` while GCC and MSVC retain the standard macro.
Do not replace the internal `log.h` guard with an identifier beginning `__`;
that namespace is reserved by ISO C and fails CERT DCL37-C.

See `docs/research/logging-consistency-audit-2026-05-30.md` for
audit that established this invariant.

### Out-parameter init functions must clear the handle on every failure

Functions with the shape `int X_init(X **out, ...)` that publish
allocation via caller's `*out` must guarantee `*out == NULL` on any
non-success return — including failure paths that take an internal `goto`
and free object before returning. Trap is combined-assignment
idiom `X *const p = *out = malloc(...);` which publishes pointer to
caller *before* later `goto free_*` paths free it.

If caller stores handle in a long-lived context (e.g.
`VmafContext.cuda.ring_buffer`), natural teardown (`vmaf_close()` →
`X_close(*out)`) will then UAF on freed object. Fix is mechanical:
set `*out = NULL` after every `free()` in failure-cleanup chain (and
explicitly on early-malloc-failure path even though assignment
already stored NULL there). Contract this pins: "caller may inspect
`*out` only on success; a non-zero return guarantees `*out == NULL`."

Pattern: see `vmaf_gpu_picture_pool_init` in
[`gpu_picture_pool.cpp`](gpu_picture_pool.cpp). Regression test:
`core/test/test_gpu_picture_pool_uaf.c`. Since the HISS-21 burn-down that
file carries no `goto`; the clearing now happens in `gpu_pool_destruct`.

### Teardown owners replace the cleanup ladders (HISS-21)

`picture.c`, `picture_pool.c`, `picture_pool.cpp`, `gpu_picture_pool.cpp`,
`predict.c`, `read_json_model.c`, `mcp/mcp.c` and `interop/` hold no `goto`.
Each former label chain is now one named `static` teardown owner, or a guard
clause that unwinds inline:

- `pool_destruct_partial(p, stage)` — `picture_pool.c` and `picture_pool.cpp`.
  `stage` counts completed acquisitions; guards run newest-first, so stage N
  frees what old label N freed, in old order. New resource: append one stage
  at the end of the enum plus one guard at the **top** of the helper.
- `gpu_pool_destruct` — `gpu_picture_pool.cpp`.
- `pool_return_index` / `pool_pop_slot` / `pool_attach_priv` — pool fetch.
  Only `pool_return_index` touches the free list, so the push-back and the
  `pthread_cond_signal` of ADR-0960 stay in one place.
- `mcp_uds_listen` / `mcp_uds_publish` — `mcp/mcp.c`. Both leave
  `atomic_store(&server->uds_running, 0)` to the caller, so that store stays
  last on every failure.

Rebase rule: a conflict must not reintroduce an early `return` between an
acquisition and its owner, and must not reorder the guards. Free order is the
contract; `core/test/test_picture_pool_error_paths.c`,
`test_picture_pool_cpp_error_paths.c` and `test_gpu_picture_pool_partial_init.c`
pin it.

`vmaf_gpu_picture_pool_init` returns `-ENOMEM` on `malloc` failure with `*pool == nullptr`.
Never return success (`0`) or leave `*pool` un-cleared on pool struct or picture array
allocation failure. Pinned by `core/test/test_gpu_picture_pool_alloc_failure.c` (#1455).

`predict.c` and `interop/pelorus_interop.c` hold scoring arithmetic. Helpers
there were cut at statement boundaries only. Never split one arithmetic
expression across a helper, and never reorder an accumulation: FMA contraction
and re-association both move scores (ADR-1253).

`predict.c::piecewise_linear_mapping` rejects non-finite input before writing
its `0.0` initialization (ADR-1302). Every ordered segment comparison is false
for NaN, so moving that initialization back above the guard converts a failed
model computation into a successful zero prediction. The production path also
routes the post-denormalization, polynomial and piecewise results through
`predict_validate_finite`; it emits one warning naming the frame and value and
returns before collector publication. The regressions require the caller-owned
output to remain unchanged on `-EINVAL`, no model score in the collector, and
exactly one diagnostic rather than one warning per mapping segment. The
`VMAF_PREDICT_TEST_NONFINITE_LOG` definition is a direct-source-inclusion test
seam only; normal builds leave it undefined and must retain the human-readable
`vmaf_log` warning.

Two `interop/pelorus_interop.c` invariants that the split introduced, both
pinned by the ADR-1142 clang-tidy ratchet (the file's allowance is 7):

- `blob_validate_framing` publishes the `const PelorusSideData *` it already
  derived. `pel_blob_find_section` consumes that pointer instead of casting the
  image bytes to a header a second time, so the blob is cast to its header in
  exactly one place per constness. Re-deriving it locally costs one extra
  `bugprone-casting-through-void` and breaks the ratchet.
- `qp_cell_average` holds the per-cell block fold. It exists so that
  `qp_fold_blocks_to_cells` stays inside the `readability-function-size`
  `NestingThreshold` of 4 — inlining it back puts the innermost statement at
  level 5. Its `int64_t sum` accumulates row-major over the clamped block
  window and the division truncates, so any reordering moves cell values
  (ADR-1253).
- `validate_pack_args` takes `out_len` as `const size_t *`: it inspects the
  caller's out-parameters for NULL and never writes through them
  (`readability-non-const-parameter`). `pel_blob_pack` still owns both stores.

### PREV_REF batch dispatch: unref before memset, zero f->prev_ref (ADR-1072)

`threaded_extract_batch_func` in `libvmaf.c` feeds PREV_REF extractors by
copying `f->prev_ref` into `fex->prev_ref` via a bare struct copy (no
`vmaf_picture_ref` — VmafRef* is shared, not reference-counted
separately).  After `vmaf_feature_extractor_context_extract()`:

- **SUCCESS**: PREV_REF SWAP in `feature_extractor.cpp` has decremented
  old-frame VmafRef (via struct-copy alias) and bumped current
  frame into `fex->prev_ref` with an extra refcount.
- **ERROR**: `fex->prev_ref` is unchanged (still struct-copy alias).

**In both cases**: call `vmaf_picture_unref(&fex->prev_ref)` before
`memset(&fex->prev_ref, 0, ...)` to release that counted reference.  Then
call `memset(&f->prev_ref, 0, ...)` to prevent `unref:` block at
bottom of function from double-freeing now-consumed VmafRef.

Bare `memset` without prior unref leaks one picture-pool slot per
PREV_REF frame, exhausting pool and deadlocking
`vmaf_picture_pool_fetch` in `pthread_cond_wait` after ~pool_size frames.

Serial path (`read_pictures_dispatch_one`) uses `vmaf_picture_ref` for
the copy (ADR-0778) and already calls `vmaf_picture_unref` before memset;
these two paths must stay consistent.

**Rebase-sensitive**: any branch that re-opens or modifies
`VMAF_FEATURE_EXTRACTOR_PREV_REF` block in `threaded_extract_batch_func`
must preserve both unref-before-memset and zero-f->prev_ref.

### SYCL shared uploads finish before either picture cleanup returns (BUG-040)

`vmaf_sycl_shared_frame_upload()` reads the caller's host-backed reference and
distorted pictures asynchronously on the in-order `copy_queue`. Its saved
`last_upload_event` is the final distorted-plane copy, so waiting on that one
event also orders every earlier reference and distorted copy without draining
the independent compute queue.

Both ownership exits in `libvmaf.c` must preserve that wait:

- `read_pictures_frame_cleanup()` waits before its direct picture unrefs.
- `threaded_read_pictures_batch()` waits after enqueue while the caller's
  original counted references are still live, then unrefs those references.
  The worker may finish and drop its own copies before the wait, so moving the
  barrier to `read_pictures_frame_cleanup_after_batch()` is too late: the
  release callback can already have poisoned or recycled the host storage.

Do not replace either event wait with a global queue/device wait, and do not
remove the threaded wait because one timing sample happened to let DMA finish
before the worker. In a combined CUDA+SYCL build, the wait must remain before
CUDA's host-cleanup early return. `core/test/test_sycl_cuda_serial_upload_lifetime.c`
pins that compile combination through the public API with `n_threads=0` and
`n_threads=1`; the 4K release callback poisons host storage as soon as its final
reference drops and PSNR proves DMA already consumed the original pixels.
`testdata/test_sycl_4k_repeat_determinism.py` then covers 20 serial and 20
`--threads 1` runs against the full normalized score report.

### framesync producer-error paths must call vmaf_framesync_abort (ADR-1092)

`retrieve_filled_data` waits in `pthread_cond_wait` until matching
BUF_FILLED entry appears.  If producer thread exits without calling
`submit_filled_data` for index consumer is waiting on, consumer
hangs forever.

Any code path that acquires framesync buffer (via
`vmaf_framesync_acquire_new_buf`) and then returns error before calling
`vmaf_framesync_submit_filled_data` for that index **must** call
`vmaf_framesync_abort(fs_ctx)` first.  This sets `aborted` flag and
broadcasts on condvar, causing all blocked `retrieve_filled_data` callers
to return `-ECANCELED`.

`vmaf_framesync_destroy` calls `vmaf_framesync_abort` as safety net, but
relying on that alone delays wake-up until destroy time, which is after
`vmaf_thread_pool_wait` — meaning thread pool wait would still hang.

**Rebase-sensitive**: any branch adding new framesync producer paths (feature
extractors, GPU dispatch loops) must include `vmaf_framesync_abort` call on
all error exits from `extract()` or equivalent before returning.

### Pooling: accumulators stay O(1) and byte-identical; percentiles buffer (ADR-1188)

`pool_accumulate()` in `libvmaf.c` feeds two consumers from one frame walk:
O(1) `PoolAccumulators` used by `MIN` / `MAX` / `MEAN` / `HARMONIC_MEAN`, and —
only when caller asked for `MEDIAN` / `PERC5` / `PERC10` / `PERC20` —
`PoolSamples` buffer holding every pooled per-frame score.

Three invariants rebase or follow-up branch must preserve:

1. **Never derive accumulator methods from sample buffer.** Their float
   expressions are upstream ones and are pinned by Netflix golden gate
   (ADR-1118 golden-gate isolation). Summing sorted vector instead would
   change summation order and move golden numbers.
2. **Never allocate sample buffer unconditionally.** Callers legitimately
   pass `index_high == UINT_MAX`; buffer must stay opt-in per method and
   geometrically grown, never sized from `index_high - index_low`.
3. **Percentiles are order statistics and ignore perceptual weighting** —
   ADR-1118 weights change `MEAN` / `HARMONIC_MEAN` only, exactly as `MIN` /
   `MAX` are unaffected. Never weight ranks.

`vmaf_percentile()` / `vmaf_score_compare()` live in `percentile.h` as `static
inline` on purpose: `predict.c` computes the golden-asserted bootstrap `ci_p95`
bounds with same expression. Keeping it header-inline keeps that
arithmetic inside `predict.c`'s own translation unit rather than behind
cross-TU call whose contraction could differ under `-flto` (ADR-1172). Never
"clean this up" into `percentile.c`.

XML / JSON writers in `output.cpp` iterate `pool_report_order[]`, **not**
`[1, VMAF_POOL_METHOD_NB)`. Appending pooling enumerator must not widen
`pooled_metrics` schema by accident; add method to that table only as
deliberate, documented output change.

## Doxygen comment invariant (ADR-1096)

Following `core/src/*.h` internal headers now carry Doxygen `@brief`,
`@param`, and `@return` annotations: `framesync.h`, `thread_pool.h`,
`picture_pool.h`, `predict.h`, `fex_ctx_vector.h`, `ref.h`, `mem.h`,
`log.h`, `opt.h`, `dict.h`.

**Invariant for rebases and follow-up branches**: when adding, renaming, or
removing function signatures in these headers, update corresponding
Doxygen block in same commit. Dangling `@param` for deleted argument
or missing `@param` for new one = docs regression. Run
`doxygen Doxyfile 2>&1 | grep warning` to check — zero new warnings =
bar.
