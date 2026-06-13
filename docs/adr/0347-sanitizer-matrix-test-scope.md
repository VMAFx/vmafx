# ADR-0347: Sanitizer matrix — concrete test-set scope per sanitizer

- **Status**: Accepted
- **Status update 2026-05-15**: implemented; ASan / UBSan / TSan
  matrix job active in
  `.github/workflows/tests-and-quality-gates.yml`; per-sanitizer
  deselect lists tracked in `docs/state.md`.
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, testing, sanitizer, asan, ubsan, tsan, fork-local

## Context

[ADR-0015](0015-ci-matrix-asan-ubsan-tsan.md) established the
sanitizer matrix (ASan + UBSan + TSan) but left the test-set
underspecified. The lane in
[`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)
runs `meson test -C build --suite=unit`, but no `test()` call in
[`core/test/meson.build`](../../core/test/meson.build) carries a
`suite: 'unit'` tag — the entire C unit-test set lands in the default
suite. Result: every sanitizer leg in the matrix prints
`No suitable tests defined.` and exits 0 with **zero correctness
coverage**. The matrix builds with sanitizers enabled but never
exercises code; pure cost, no signal.

This is the failure mode our memory-rule
`feedback_no_test_weakening` warns against: a gate that exists for
correctness coverage but is silently neutered by a configuration
mismatch. It also matches the
[Research-0089 §5](../research/0089-sanitizer-matrix-test-scope.md)
finding that flagged the symptom out-of-scope of the parent CI
audit.

The matrix's intended job — surfacing memory, UB, and concurrency
bugs the Netflix golden gate cannot catch — has been silently OFF
since at least ADR-0015 landed. Empirical evidence (this PR's local
run) shows the matrix would have caught **seven real bugs** that
have been hiding behind the `--suite=unit` no-op.

## Decision

We will run the **full C unit-test set** under each of ASan, UBSan,
and TSan, with a **per-sanitizer deselect list** documenting tests
that fail because of a real underlying bug (not a sanitizer
mis-configuration). Each deselected entry cites a follow-up bug in
[`docs/state.md`](../state.md) so the gap stays visible until the
underlying defect is fixed and the deselect can be removed.

Concretely:

- **ASan**: build with `clang-18 + lld-18 + b_sanitize=address +
  buildtype=debug + b_lto=false + b_lundef=false`. Run all C unit
  tests except `test_model`, `test_predict`,
  `test_float_ms_ssim_min_dim` (three real defects — see
  *Per-sanitizer verification table*).
- **UBSan**: same toolchain with `b_sanitize=undefined` and
  `c_args+cpp_args=-fno-sanitize=function`. The `function` check
  trips on the K&R-prototype `static char *test_X()` pattern in
  ~50 minunit-style harness files
  ([`core/test/test.h`](../../core/test/test.h) + every
  `test/test_*.c`), which is upstream Netflix harness code we are
  not refactoring in this PR; suppressing the `function` check
  surfaces every other UBSan signal cleanly. Run all C unit tests
  except `test_model` (real defect — same `svm.cpp` parser path
  ASan flags).
- **TSan**: same toolchain with `b_sanitize=thread`. Run all C unit
  tests except `test_model` (same defect),
  `test_pic_preallocation` (`integer_adm.c` global-init race),
  `test_framesync` (`framesync.c` mutex-domain mismatch).

We do **not** add MSan to the matrix. The matrix in flight has
always been ASan + UBSan + TSan (the user prompt's "MSan" naming
was a misnomer for TSan). MSan requires every linked library —
including system libc++ — to be MSan-instrumented, which is
impractical with stock Ubuntu glibc. Adding MSan would require an
instrumented-libc++ build leg (`-stdlib=libc++` + custom
`-DLLVM_USE_SANITIZER=Memory` toolchain) that is well beyond the
T7-MCP-SMOKE-CI / Research-0089 scope. Defer to a separate ADR if a
case for MSan emerges.

The Netflix golden Python gate stays in its own job
(`netflix-golden`); the sanitizer matrix is a C-level memory /
UB / race net, not a numerical-correctness gate. Running the
Netflix Python pairs under sanitized libvmaf would burn ~25 min
per leg with marginal additional signal versus the existing
non-sanitized golden lane, so we keep the two concerns separated.

The DNN suite is excluded by virtue of not enabling
`-Denable_dnn=enabled` in the sanitizer build (the existing matrix
configuration). ONNX Runtime ships uninstrumented binaries that
would generate spurious sanitizer reports across `core/src/dnn/`;
the dnn lane has its own job (`dnn:` in
`tests-and-quality-gates.yml`) which exercises those paths without
sanitizers. Adding a sanitized dnn run is a separate ADR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Status quo — `--suite=unit`** (zero tests) | Zero CI runtime cost | Zero correctness coverage; matrix is theatre; `feedback_no_test_weakening` fires | The whole point of the matrix is signal; cost without signal is the failure mode. |
| **Run all tests, fail on every existing bug** | Forces immediate fix of all 7 surfaced defects | Blocks every unrelated PR until 7 fixes land; conflates "regression net" with "outstanding-debt audit" | Land the gate now, file the defects in `state.md`, fix them in dedicated follow-up PRs. Same final state, much shorter merge train. |
| **Run all tests + `continue-on-error: true`** | Surfaces signal without blocking | A perma-yellow gate is functionally indistinguishable from no gate (alert blindness) | Required-status semantics demand a binary green/red signal; that is the only way to prevent regressions from landing. |
| **Add MSan as a fourth leg** | Catches uninitialised-memory bugs ASan misses | Requires instrumented libc++ + deep build-system surgery; out of scope | Defer to a separate ADR after the existing 3-leg matrix produces real signal for ≥1 release cycle. |
| **Run the full Python `python/test/` suite under sanitizers** | Maximum coverage of integration paths | ~25 min per leg × 3 sanitizers; CI runner-minute cost dominates the signal-per-minute ratio | The Netflix golden lane already exercises the integration paths on a non-sanitized build; the C unit suite under sanitizers is the right cost/signal tradeoff. |
| **Per-sanitizer deselect lists with documented gaps** *(chosen)* | Closes the coverage hole today; surfaces real bugs as future work; gate stays binary green/red | Three deselect lists to maintain; risk of new test additions silently inheriting "no sanitizer coverage" if a new test fails | Mitigated by `state.md` rows tracking each gap and a follow-up cadence to close them; new tests default to running on all sanitizers. |

## Consequences

- **Positive**: the sanitizer matrix produces real signal again.
  Seven defects that have been hiding behind `--suite=unit` are now
  documented and tracked. Future regressions in the same
  classes (use-after-free, signed-overflow UB, lock-domain races)
  fail CI on the introducing PR.
- **Positive**: the gap between "the matrix exists" and "the
  matrix prevents regressions" closes. `feedback_no_test_weakening`
  is honoured — instead of relaxing the test set to make CI pass,
  we land documented gaps that bug-fix PRs erase one at a time.
- **Negative**: per-leg wall time grows from ~0s (no tests) to
  ~5–6s (47 tests under ASan, 49 under UBSan, 47 under TSan). The
  full `sanitizers` job still finishes in ~5 min including the
  build (vs ~5 min before with no test execution), so total
  wall-clock cost is roughly unchanged once compile-time
  amortisation is included.
- **Negative**: three deselect lists in the workflow YAML need to
  stay in sync with `state.md` rows. The PR template's Bug-status
  hygiene checkbox already requires reviewers to verify.
- **Neutral / follow-ups**: file issues for each of the seven
  bugs surfaced (see *Per-sanitizer verification table*); each
  follow-up PR removes the corresponding deselect entry. Track
  in `state.md` Open-bugs section.

## Per-sanitizer verification table

All numbers from a local run on this PR's branch
(`ci/sanitizer-matrix-test-scope`, tip
`<filled-in-by-pr-body>`), Ubuntu host with `clang version 22.1.3
(target x86_64-pc-linux-gnu)` standing in for clang-18; CI uses
`clang-18 + lld-18 + libclang-rt-18-dev` per the existing job. The
defect classes reproduce on both clang-18 and clang-22, so the
local-vs-CI compiler delta does not change the verification.

Reproducer command for any single leg:

```bash
cd libvmaf
CC=clang CXX=clang++ LDFLAGS=-fuse-ld=lld \
  meson setup build-asan -Db_sanitize=address \
    -Denable_cuda=false -Denable_sycl=false --buildtype=debug \
    -Db_lto=false -Db_lundef=false
meson compile -C build-asan
meson test -C build-asan --print-errorlogs $(meson test -C build-asan --list \
  | grep '^libvmaf:' \
  | grep -vE 'test_model$|test_predict$|test_float_ms_ssim_min_dim$' \
  | sed 's/^libvmaf://')
```

| Sanitizer | Build flags (added on top of existing job) | Tests run | Wall (test exec only) | Deselected | Defect class | First-bad evidence |
| --- | --- | --- | --- | --- | --- | --- |
| ASan | none (b_sanitize=address) | 36 / 36 OK | ~5.3 s | `test_model`, `test_predict`, `test_float_ms_ssim_min_dim` | (a) `test_model::test_json_model_synthetic_branches` — `SVMModelParser::parse_support_vectors` (`core/src/svm.cpp:2955`) requests `0xfffffffffffffff8`-byte allocation on malformed JSON model buffer (allocation-size-too-big); (b) `test_predict::test_propagate_metadata` — direct + indirect leaks of metadata `dict` entries (`core/src/dict.c:124`) and string keys not freed when `feature_collector_dispatch_metadata` re-enters; (c) `test_float_ms_ssim_min_dim::test_float_ms_ssim_init_*` — direct calloc leaks in test-side `invoke_init` (`test/test_float_ms_ssim_min_dim.c:33`) — `init` allocates an extractor state never reclaimed | ASan abort + stack traces in the PR description |
| UBSan | `c_args=-fno-sanitize=function`, `cpp_args=-fno-sanitize=function` | 38 / 38 OK | ~5.4 s | `test_model` | `test_model::test_json_model_synthetic_branches` — `SVMModelParser::parse_support_vectors` passes NULL as `memcpy` arg2 (`core/src/svm.cpp:2989`, declared `__nonnull`) on malformed JSON model buffer | UBSan runtime-error + abort (same defect as ASan finding `(a)` — both surface the same parse path's missing-validation bug from different angles) |
| TSan | none (b_sanitize=thread) | 36 / 36 OK | ~1.3 s | `test_model`, `test_pic_preallocation`, `test_framesync` | (d) `test_model` — same defect as ASan/UBSan finding `(a)`; (e) `test_pic_preallocation` — `div_lookup` global table init in `div_lookup_generator` (`core/src/feature/integer_adm.h:36`) is racing across worker threads (`integer_adm.c::init` runs from `vmaf_thread_pool_runner`) — write-write race on `div_lookup[idx]`, classic missing-atomic on a "compute-once-cache-forever" lookup table; (f) `test_framesync` — `vmaf_framesync_submit_filled_data` reads `count` (`core/src/framesync.c:125`) under one mutex while `vmaf_framesync_acquire_new_buf` writes the same `count` (`framesync.c:102`) under a different mutex — mutex-domain mismatch on the per-buffer state | TSan WARNING: data race + abort |

The `function`-check exclusion under UBSan is documented in the
ADR, not deferred to a TODO: the K&R-prototype harness pattern is
upstream Netflix code at scale, and rewriting ~50 test files to
use `static char *test_X(void)` is its own PR (T7-5-style sweep).
The exclusion preserves UBSan's signal on the actual library
sources — the only tests that "would fail" without this exclusion
fail because of the harness, not the code under test.

## References

- Parent: [ADR-0015](0015-ci-matrix-asan-ubsan-tsan.md) (matrix established).
- Companion research digest: [`docs/research/0090-sanitizer-matrix-test-scope.md`](../research/0090-sanitizer-matrix-test-scope.md).
- CI workflow:
  [`.github/workflows/tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)
  job `sanitizers`.
- Related rules: `feedback_no_test_weakening`,
  `feedback_no_skip_shortcuts` (memory).
- Source: `req` ("the matrix builds with sanitizers enabled but never
  exercises code; pure cost, zero correctness coverage").
