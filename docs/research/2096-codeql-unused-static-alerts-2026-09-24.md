<!-- markdownlint-disable MD013 MD060 -->
# Research-2096: CodeQL unused-static-function identity audit

- **Status**: Active
- **Workstream**: [ADR-1142](../adr/1142-whole-codebase-standards.md), CodeQL alert closure
- **Last updated**: 2026-09-25

## Question

Whether the `cpp/unused-static-function` findings in `core/src/pdjson.c`,
`core/src/picture.c`, `core/src/thread_pool.c`, `core/src/predict.c`,
`core/src/feature/cambi.c`, and `core/test/test_fex_ctx_vector.cpp` identify dead implementation code, and how
to close them without deleting live behavior, changing public ABI, suppressing
the CodeQL unused-static findings, or weakening tests that compile
implementation sources under special configurations.

## Sources

- GitHub Code Scanning alerts API for `VMAFx/vmafx`, queried 2026-09-24.
- Hosted C/C++ CodeQL database ID `543508132`, created from master commit
  `4e6916d16ac57647105d14a47a6680117d6b5738`.
- CodeQL CLI 2.27.0 and `codeql/cpp-queries` 1.8.3, matching the repository
  workflow.
- [CodeQL query help: Unused static function](https://codeql.github.com/codeql-query-help/cpp/cpp-unused-static-function/).
- [CodeQL query source](https://github.com/github/codeql/blob/main/cpp/ql/src/Best%20Practices/Unused%20Entities/UnusedStaticFunctions.ql).
- `core/test/meson.build`, `core/src/pdjson.c`, `core/src/picture.c`,
  `core/src/thread_pool.c`, `core/src/predict.c`,
  `core/src/feature/cambi.c`, and the owning tests.

## Findings

### Alert inventory

The live inventory covered alerts 1066-1098 plus 1230. Of the selected findings,
25 were open before this change: 21 in `pdjson.c`, three in `thread_pool.c`, and
`vector_unchanged` in `test_fex_ctx_vector.cpp`. Eight selected pdjson findings
were already dismissed (1074, 1075, 1077, 1078, 1079, 1081, 1088, and 1089).
Alert 1097, on the test interposer `observed_create`, was also already dismissed
but is not one of the selected production-function findings. This change does not
create, reopen, or alter any dismissal.

The exact query does not define reachability as "called from `main`". Its first
roots are non-static functions, plus explicitly annotated used symbols. The
failure was instead an extraction identity seam: the same implementation file
was compiled into many link targets. CodeQL coalesced repeated external
definitions while retaining link-target-specific static helper graphs. Calls
could therefore attach to one compiled identity while an equivalent helper graph
in another target appeared orphaned.

### `pdjson.c`

Four tests compiled redundant copies of `pdjson.c`: `test_predict`, `test_model`,
`test_model_libsvm_dup_key`, and `test_model_feature_overload_ownership`. Those
targets already link the library or the JSON model reader and do not own a
pdjson implementation copy, so the redundant sources were removed.

Three direct copies are intentional and remain: the ordinary `test_pdjson` and
the `PDJSON_STACK_INC=0` / `SIZE_MAX` growth-failure binaries. The latter two are
required by the `core/src` test invariant and cannot be replaced by the ordinary
library object. Each intentional copy is now a target-local static library. Its
private helper names are unique to that copy, while the public `json_*` API stays
unchanged; this keeps the external API graph truthful to whole-program analyzers
such as cppcheck. Each executable separately renames `run_tests`, so the default,
zero, and oversized test bodies have distinct non-static roots. These aliases
are target-local Meson compiler arguments and change neither the production
object nor the installed ABI.

Adding broad scalar, stream, or container tests did not fix this class of
finding: runtime coverage does not change CodeQL's repeated-definition identity
model. The specialized test remains focused on the invalid stack-increment
contract and its existing cleanup behavior.

### `thread_pool.c`

`test_picture` and `test_picture_v2` compiled `thread_pool.c` without exercising
it, so those redundant source entries were removed. The three open hosted alerts,
however, mapped to the deliberately source-including
`test_thread_pool_backpressure` target, not the picture tests. That test must keep
its private copy to interpose deterministic pthread failures.

Before including `thread_pool.c`, the backpressure test now gives
`vmaf_thread_pool_create`, `destroy`, `enqueue`, and `wait` unique test-local
names. The aliases stay active through the test body, so definitions and calls
refer to the same isolated external roots while the production library keeps its
original symbols. The pthread interposition and all four runtime cases are
unchanged.

This test also carries one narrowly scoped cppcheck-only annotation on
`observed_destroy`: `constParameterPointer` is suppressed because the wrapper
must retain `pthread_cond_destroy`'s mutable `pthread_cond_t *` contract for the
macro interposition, then pass that pointer to the POSIX function without a
qualifier-discarding cast. The annotation cites this digest and does not suppress,
exclude, or dismiss any `cpp/unused-static-function` result.

### `test_fex_ctx_vector.cpp`

`vector_unchanged` belongs only to the non-LTO allocation-failure harness. Its
call sites were already correctly guarded by `FEX_VECTOR_ALLOC_TEST`, but its
definition existed in the default LTO build and relied on `[[maybe_unused]]`.
The definition now lives under the same configuration guard as its real users.
Default builds no longer emit dead test code, while the non-LTO harness still
builds and exercises the helper. No unrelated predicate test was added merely
to satisfy the scanner.

### `picture.c`

Standards review found that a fresh full-database extraction produces two query
rows in `core/src/picture.c`: `picture_compute_geometry` (line 137) and
`pool_release_picture` (line 105).

Inspection of the compile commands revealed a translation-unit identity split:
most copies of `picture.c` (in `libvmaf` and the 50 coverage test targets) link
as one identity under `vmaf_cflags_common` (`-fvisibility=hidden -DVMAF_BUILDING_LIBVMAF`).
However, `test_picture`, `test_picture_v2`, and `test_picture_pool_error_paths`
compiled direct source copies without those flags, creating a second translation-unit
identity. When CodeQL coalesced the external function `vmaf_picture_alloc`, the single
coalesced function body attached to the ordinary library identity where calls reached
its static helpers. The second uncoalesced identity's static helpers
(`picture_compute_geometry` and `pool_release_picture`) were left with 0 callers.

`test_picture`, `test_picture_v2`, and `test_picture_pool_error_paths` now share
one uniquely-owned test-local static library, `test_picture_impl`, for
`picture.c`, `mem.cpp`, and `ref.cpp`. The error-path target still compiles
`picture_pool.c` directly, preserving the ADR-0960 seam that exposes its
internal pool entry points. The sibling C++ error-path target was not part of
the orphan identity and remains unchanged.

### Exact-base predictor follow-up

A fresh database from exact base
`71c3c155717f4496c3c572e479d5202984e9c3b5` corrected the prior four-row
remainder. The official interpreted CSV contained five rows from two predictor
identity seams:

1. `post_process_feature_from_another`
2. `scan_feature`
3. `scan_match_feature`
4. `predict_validate_finite`
5. `test_predict_nonfinite_log`

A diagnostic CodeQL table query showed two definitions at the same source
location for both `post_process_feature_from_another` and
`predict_validate_finite`. The exact-base compile database contained 54
`predict.c` commands: the library, 52 private-source test targets with common
production flags, and `test_feature_collector` without those flags. Removing
only the collector copy did not close the three scan rows: a second fresh
database still reported them. The remaining 52 test copies were enough for
CodeQL to coalesce external predictor roots while retaining an orphan private
scan graph. Separately, `test_predict.c`'s textual implementation include
created the orphan `predict_validate_finite` identity, whose test-only log
callback was reachable only from that root. These are the same
coalesced-external-root / duplicated-static-graph failure mode as the earlier
test targets, not dead production behavior.

The same exact-base database contained one `cambi_score` definition and one
call to `dump_c_values`; the official query emitted no CAMBI row. The previously
recorded CAMBI remainder was stale by this base and required no source change.

The predictor test now shares only the pure linear, piecewise, and bitwise
equality helpers through `predict_internal.h`. Their expression text and
evaluation order moved unchanged as `static inline` definitions. End-to-end
tests link the production predictor instead of unity-including it, and the
non-finite test runs the real `vmaf_log` path. A source-authority test prevents
the implementation include from returning; a subprocess-level contract checks
that the production warning names the stage and frame and appears exactly once.
`test_feature_collector` never scores a model, so its redundant predictor copy
was removed. Like the wider private-source test graph, the target now links
`predict_c_dependency`: `predict_c_lib` is the single compile authority, and
libvmaf extracts the same object. The CPU compile database consequently
contains one `predict.c` command instead of 54.

## Alternatives considered

| Approach | Result | Decision |
| --- | --- | --- |
| Delete the reported helpers | Removes live parser, worker, or allocation-failure behavior | Rejected |
| Suppress or dismiss the findings | Hides the extraction seam and leaves the test graph ambiguous | Rejected |
| Add broad runtime-only parser tests | Does not distinguish repeated compiled identities; adds unrelated test and cleanup surface | Rejected |
| Remove the macro-specialized pdjson tests | Loses the required zero/overflow stack-growth regression contracts | Rejected |
| Rename production symbols or change public headers | Unnecessary ABI churn for a test-build problem | Rejected |
| Alias every public API in each intentional copy | Closes CodeQL, but manufactures unused external APIs for cppcheck | Rejected |
| Remove redundant copies and uniquely name intentional private helpers | Preserves behavior, the public API, and an unambiguous helper graph for both analyzers | **Chosen** |
| Share `test_picture_impl` with all three orphan targets | Gives the helpers one called identity while preserving direct `picture_pool.c` error-path access | **Chosen** |
| Delete the predictor scan helpers | Breaks live chroma-from-luma correction | Rejected |
| Mark duplicate predictor helpers `used` / `unused` | Suppresses the query without repairing the duplicate identity | Rejected |
| Keep the unity include and add unrelated calls | Manufactures reachability and still tests a private copy instead of production logging | Rejected |
| Share pure helpers through an internal header and link the production predictor | Preserves exact arithmetic, removes the duplicate implementation identity, and exercises the shipped diagnostic | **Chosen** |
| Remove only the collector target's direct predictor copy | Closes one redundant compile but leaves 52 test copies and the scan findings | Rejected |
| Inline or export the three reported scan helpers | Alters good implementation structure only to change query reachability | Rejected |
| Keep compiling `predict.c` in every private-source target | Repeats a production TU 53 extra times and leaves ambiguous static identities | Rejected |
| Compile `predict.c` once as an internal static library and link every consumer | Preserves C linkage and runtime behavior while giving analyzers and builds one implementation identity | **Chosen** |

## Verification evidence

The hosted master database was replayed with the workflow's exact query:

```text
Best Practices/Unused Entities/UnusedStaticFunctions.ql
CodeQL CLI 2.27.0; codeql/cpp-queries 1.8.3
master: 4e6916d16ac57647105d14a47a6680117d6b5738
```

A fresh branch database was then extracted from a clean CPU build configured
with `-Denable_cuda=false -Denable_sycl=false`. Before the picture correction,
the exact query returned **six repository-wide rows**: two in `picture.c`, three
in `predict.c`, and one in `cambi.c`. This corrected the earlier five-row claim,
which had counted raw BQRS entities rather than interpreted CSV findings.

The official CodeQL CSV output (`codeql database analyze --format=csv`) was
parsed and validated fail-closed with `scripts/ci/check-codeql-unused-static.py`.
The `codeql-unused-static-schema-contract` pre-commit hook exercises its
positive, target-violation, raw-BQRS, and malformed-schema fixtures in CI.
The six-row pre-correction inventory was:

1. `core/src/picture.c:137`: `picture_compute_geometry` (orphan test identity; fixed here)
2. `core/src/picture.c:105`: `pool_release_picture` (orphan test identity; fixed here)
3. `core/src/predict.c:309`: `post_process_feature_from_another` (volatile duplicate-identity group)
4. `core/src/predict.c:282`: `scan_feature` (volatile duplicate-identity group)
5. `core/src/predict.c:262`: `scan_match_feature` (volatile duplicate-identity group)
6. `core/src/feature/cambi.c:1512`: `dump_c_values` (pre-existing debug helper)

After all three orphan targets were consolidated, the same interpreted query
returned **four repository-wide rows**: the three `predict.c` rows and the one
`cambi.c` row above. It returned zero rows in every selected lane path:
`picture.c`, `pdjson.c`, `thread_pool.c`, `test_fex_ctx_vector.cpp`, and
`test_thread_pool_backpressure.c`. This is deliberately not reported as a
repository-wide zero.

The 2026-09-25 exact-base replay superseded that volatile remainder: it returned
five predictor rows: three from the repeated private-source build graph and two
from the newer non-finite unity-test seam. It emitted no CAMBI row. This
inventory was captured before changing either test boundary; the
source-authority and production-log contracts both failed against the old
unity include.

The first staged replay removed the unity include and the collector-only copy.
It closed `predict_validate_finite` and `test_predict_nonfinite_log`, but still
returned the three scan rows. That falsified the narrow collector attribution
and led to the compile-database audit: 53 remaining `predict.c` commands (one
library plus 52 tests). Consolidating those commands, rather than changing the
live scan helpers, is the final repair.

The final clean replay compiled the complete CPU graph in 1,559 steps with one
`predict.c` command. CodeQL CLI 2.27.0 evaluated the official
`codeql/cpp-queries` 1.8.3 `UnusedStaticFunctions.ql` against that newly
extracted database and produced a zero-byte CSV: **zero repository-wide rows**.
No CAMBI source changed. The four predictor/collector authority tests passed
4/4 in the extracted build; the equivalent final Meson graph passed 180/181
tests with one expected skip and no failures. A no-LTO CPU clang-tidy replay
measured the touched predictor test at 13 warnings, so the generated CPU
ratchet was tightened from 14 to 13 with the scoped writer.

Focused runtime verification after the identity repair:

```text
test_pdjson                              PASS
test_pdjson_stack_increment_zero         PASS
test_pdjson_stack_increment_oversized    PASS
test_thread_pool_backpressure             PASS
test_fex_ctx_vector                       PASS
test_picture                              PASS
test_picture_v2                           PASS
test_picture_pool_error_paths             PASS
test_picture_pool_cpp_error_paths         PASS
```

The complete CPU build then passed all 160 Meson tests. The non-LTO vector
configuration passed all 14 allocation-path cases (the default configuration
passed its 11 cases), and the Netflix CPU golden gate finished with 271 passed
and 12 skipped. The configured 11-command pdjson/backpressure cppcheck slice was
clean under `--enable=all --check-level=exhaustive`; direct warnings-as-errors
clang-tidy checks were clean for both changed test sources. `make verify-all`,
`make format-check`, the changed-document markdown lint, and the fragment checks
also passed.

The repository-wide CPU tidy-ratchet invocation could not produce a measurement:
the local clang-tidy rejects GCC 16's generated `-flto=4` argument before parsing
every translation unit. The prepared analyzer database normalizes that spelling
to `-flto`; the two touched source checks passed there. This toolchain-wide
failure is not counted as a successful gate or as evidence about this change.

Hosted alert closure remains pending until GitHub analyzes the merged default
branch. The local replay proves the branch result under the same CLI, query pack,
configuration, and full-build extraction shape; it does not substitute for that
post-merge receipt.

## Open questions

- Whether GitHub deduplicates every entity-level result back to the same 25 open
  alert records can only be confirmed by the fresh hosted default-branch run.
- Hosted closure still depends on a fresh default-branch analysis after merge;
  local exact-query evidence cannot update GitHub's alert records.

## Related

- [ADR-1142](../adr/1142-whole-codebase-standards.md): whole-tree warning policy.
- `T-CODEQL-UNUSED-STATIC-FUNCTIONS-2026-09-24` in
  [`docs/state.md`](../state.md).
