<!-- markdownlint-disable MD013 -->
# Research-2078 — CodeQL unused-static-function alerts audit and reachability repair

## Executive summary

A live GitHub Code Scanning inventory on 2026-09-24 covered 34 numeric IDs: the inclusive
1066–1098 span plus alert 1230. Thirty-three are selected `cpp/unused-static-function`
(CWE-561) findings across `core/src/pdjson.c` (alerts 1066–1094),
`core/src/thread_pool.c` (alerts 1095, 1096, 1098), and
`core/test/test_fex_ctx_vector.cpp` (alert 1230). Alert 1097 is the remaining ID in the
span and concerns the interposed `observed_create` test helper, not a selected production
function.

At inventory time, 25 selected findings were open: 21 in `pdjson.c`, three in
`thread_pool.c`, and alert 1230. Nine IDs in the full span had already been dismissed before
this change: 1074, 1075, 1077, 1078, 1079, 1081, 1088, 1089, and 1097. The first eight are
selected `pdjson.c` findings; 1097 is the unselected interposition finding. This change does
not alter any dismissal. The 25 open alerts remain pending until a fresh hosted CodeQL run
analyzes the branch.

A preliminary triage in `docs/state.md` (`T-CODEQL-TRAIN-FINDINGS-2026-09-16`) had dismissed
several findings as false positives under the belief that CodeQL failed to follow interposition
macros. A rigorous architectural investigation revealed the true root causes:

1. **Meson build/visibility seam leaks in test executables**:
   `core/test/meson.build` attached `../src/thread_pool.c` directly to the `test_picture` and
   `test_picture_v2` executable targets. Neither target exercises thread pools. Similarly,
   `../src/pdjson.c` was attached directly to `test_predict` (zero JSON parsing) and redundantly
   to `test_model`, `test_model_libsvm_dup_key`, and `test_model_feature_overload_ownership`
   (which already consume `read_json_model_cpp23_lib` and `libvmaf`). Because CodeQL evaluates
   reachability of static internal-linkage symbols from `main()` within each compiled executable
   target, compiling complete library implementation translation units into unrelated test binaries
   caused CodeQL to flag every static function in those files as unreachable.

2. **Incomplete reachability contract in macro-specialized test targets**:
   `test_pdjson_stack_increment_zero` and `test_pdjson_stack_increment_oversized` compile `pdjson.c`
   with `-DPDJSON_STACK_INC=0` and `-DPDJSON_STACK_INC=SIZE_MAX`. The original test only parsed
   `"[]"`, which fails immediately at `push()` before exercising any scalar decoding (numbers,
   strings with Unicode/escapes, booleans, null), user/stream I/O sources, or container traversal.
   Consequently, all other static helpers in `pdjson.c` appeared dead in those executables.

3. **Macro-gated test helper lacking unconditional reachability**:
   In `core/test/test_fex_ctx_vector.cpp`, `vector_unchanged()` was invoked only under
   `#ifdef FEX_VECTOR_ALLOC_TEST`. Because `b_lto=true` by default in `core/meson.build`, the allocation
   fault-injection harness (`fex_vector_alloc_harness`) is disabled in default builds. Marking the
   helper `[[maybe_unused]]` satisfies compiler `-Wunused-function` warnings but does not satisfy
   CodeQL's call-graph reachability analysis.

None of the selected functions in `pdjson.c`, `thread_pool.c`, or
`test_fex_ctx_vector.cpp` are truly dead code. All represent intended upstream functionality
or load-bearing test contracts. No implementation function is deleted or suppressed, and this
change neither creates nor modifies a GitHub dismissal.

## Detailed audit of alerts

### 1. `core/src/pdjson.c` (Alerts 1066–1094)

The 34 static functions in `pdjson.c` implement the core streaming parser:

- Source abstraction: `buffer_get`, `buffer_peek`, `stream_get`, `stream_peek`, `user_get`, `user_peek`
- Parser lifecycle: `init`, `finish_document`, `json_begin_error`
- Container state machine: `push`, `pop`, `read_value`, `read_array_item`, `read_object_item`, `read_member_name`
- Scalar decoding: `is_match`, `read_string`, `read_number`, `read_digits`, `read_fraction`, `read_exponent`, `is_digit`, `next_nonspace`
- String/Unicode decoding: `init_string`, `pushchar`, `read_escaped`, `read_unicode`, `read_unicode_cp`, `encode_utf8`, `hexchar`, `char_needs_escaping`, `utf8_seq_length`, `is_legal_utf8`, `read_utf8`

**Remediation**:

- Removed stray `'../src/pdjson.c'` compilation from `test_predict`, `test_model`,
  `test_model_libsvm_dup_key`, and `test_model_feature_overload_ownership`.
- Extended `core/test/test_pdjson_stack_increment.c` to test the complete reachability contract:
  - Scalar parsing without stack growth: numbers, strings with Unicode escapes (`\u0041`), escapes (`\n`), booleans, null, and streaming document separation with `json_reset()`.
  - Custom user source (`json_open_user`) and stream source (`json_open_stream`).
  - Preallocated container parsing within capacity, verifying that `push()`, `pop()`, `read_array_item()`, `read_object_item()`, `read_member_name()`, and `finish_document()` operate without reallocation.
  - Overflow rejection when container depth exceeds preallocated capacity.

### 2. `core/src/thread_pool.c` (Alerts 1095, 1096, 1098)

- Alert 1095: `pool_init_primitives`
- Alert 1096: `pool_spawn_workers`
- Alert 1098: `vmaf_thread_pool_runner`

These functions are called directly during `vmaf_thread_pool_create()` to initialize mutexes/condition
variables, spawn worker threads, and run the worker loop.
They were only flagged because `test_picture` and `test_picture_v2` compiled `../src/thread_pool.c`
without calling any thread pool API.

**Remediation**:

- Removed `'../src/thread_pool.c'` from `test_picture` and `test_picture_v2` in `core/test/meson.build`.
- Verified that `test_thread_pool` and `test_thread_pool_backpressure` thoroughly exercise all
  primitives, worker spawning, job scheduling, capacity waiting, error propagation, and runner execution.

### 3. `core/test/test_fex_ctx_vector.cpp` (Alert 1230)

- Alert 1230: `vector_unchanged`

**Remediation**:

- Removed `[[maybe_unused]]` attribute.
- Added `test_vector_unchanged_predicate()` exercising both positive (matching storage, count, capacity)
  and negative (mismatched storage, count, capacity) conditions per HISS-15 3D testing.
- Registered `test_vector_unchanged_predicate()` in `run_legacy_and_growth_tests()`, making it
  unconditionally reachable in all build configurations (`b_lto=true` and `b_lto=false`).

## Alternatives explored

`no alternatives: only-one-way fix`. Deleting live parser/thread-pool functions would remove
production behavior, and suppressing or dismissing the findings would preserve the faulty test
target reachability model. The correction is to stop compiling unrelated library translation
units into tests and to execute the intended helper/parser paths in the tests that own them.

## Reproducer and smoke commands

Inventory the exact hosted status before and after the branch receives a fresh CodeQL run:

```bash
for alert in $(seq 1066 1098) 1230; do
  gh api "repos/VMAFx/vmafx/code-scanning/alerts/${alert}" \
    --jq '[.number, .state, (.dismissed_reason // "-")] | @tsv'
done
```

Run the focused implementation smoke in an existing default build:

```bash
meson test -C core/build \
  test_picture test_picture_v2 test_model test_model_libsvm_dup_key \
  test_model_feature_overload_ownership test_predict test_pdjson \
  test_pdjson_stack_increment_zero test_pdjson_stack_increment_oversized \
  test_fex_ctx_vector test_thread_pool test_thread_pool_backpressure \
  --print-errorlogs
```

## Verification evidence

1. **Compilation**: Clean build of all 200 targets via Ninja under default build (`b_lto=true`), non-LTO (`b_lto=false`), and AddressSanitizer/UndefinedBehaviorSanitizer (`-Db_sanitize=address,undefined`).
2. **Fast test suite**: exact-base preflight passed 146/146 tests under GCC, Clang, and
   ASan + UBSan.
3. **Focused tests**:
   - `test_picture`: 10/10 passed
   - `test_picture_v2`: 10/10 passed
   - `test_model`: 62/62 passed
   - `test_model_libsvm_dup_key`: 1/1 passed
   - `test_model_feature_overload_ownership`: 8/8 passed
   - `test_predict`: 5/5 passed
   - `test_pdjson`: 9/9 passed
   - `test_pdjson_stack_increment_zero`: 10/10 passed
   - `test_pdjson_stack_increment_oversized`: 10/10 passed
   - `test_fex_ctx_vector`: 12/12 passed (default LTO) / 15/15 passed (non-LTO)
   - `test_thread_pool`: 2/2 passed
   - `test_thread_pool_backpressure`: 4/4 passed
4. **Sanitizers**: Zero leaks or memory faults across all 12 targets under ASan + UBSan.
5. **Linting and Ratchets**:
   - `make format-check`: passed cleanly.
   - Direct diagnostics in the two touched test sources: 0. The preflight wrapper also
     surfaced 55 inherited header diagnostics (one from unchanged `pdjson.h`, 54 from
     unchanged headers included by the vector test); the ratchet mapped all 55 to unchanged
     files, so no touched-source baseline was loosened.
   - `cppcheck`: 0 warnings reported.
6. **Netflix Golden Data**: `make test-netflix-golden` reported `271 passed, 12 skipped,
   1 warning`, with no golden assertion edits or score drift.
7. **Hosted CodeQL**: pending. Local reachability, link, and test evidence does not claim
   hosted closure; `docs/state.md` remains open until the fresh analysis confirms it.
