<!-- markdownlint-disable MD013 MD060 -->
# Research-2096: CodeQL unused-static-function identity audit

- **Status**: Active
- **Workstream**: [ADR-1142](../adr/1142-whole-codebase-standards.md), CodeQL alert closure
- **Last updated**: 2026-09-24

## Question

Whether the `cpp/unused-static-function` findings in `core/src/pdjson.c`,
`core/src/thread_pool.c`, and `core/test/test_fex_ctx_vector.cpp` identify dead
implementation code, and how to close them without deleting live behavior,
changing public ABI, adding scanner suppressions, or weakening the tests that
compile implementation sources under special configurations.

## Sources

- GitHub Code Scanning alerts API for `VMAFx/vmafx`, queried 2026-09-24.
- Hosted C/C++ CodeQL database ID `543508132`, created from master commit
  `4e6916d16ac57647105d14a47a6680117d6b5738`.
- CodeQL CLI 2.27.0 and `codeql/cpp-queries` 1.8.3, matching the repository
  workflow.
- [CodeQL query help: Unused static function](https://codeql.github.com/codeql-query-help/cpp/cpp-unused-static-function/).
- [CodeQL query source](https://github.com/github/codeql/blob/main/cpp/ql/src/Best%20Practices/Unused%20Entities/UnusedStaticFunctions.ql).
- `core/test/meson.build`, `core/src/pdjson.c`, `core/src/thread_pool.c`, and the
  owning tests.

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

### `test_fex_ctx_vector.cpp`

`vector_unchanged` belongs only to the non-LTO allocation-failure harness. Its
call sites were already correctly guarded by `FEX_VECTOR_ALLOC_TEST`, but its
definition existed in the default LTO build and relied on `[[maybe_unused]]`.
The definition now lives under the same configuration guard as its real users.
Default builds no longer emit dead test code, while the non-LTO harness still
builds and exercises the helper. No unrelated predicate test was added merely
to satisfy the scanner.

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

## Verification evidence

The hosted master database was replayed with the workflow's exact query:

```text
Best Practices/Unused Entities/UnusedStaticFunctions.ql
CodeQL CLI 2.27.0; codeql/cpp-queries 1.8.3
master: 4e6916d16ac57647105d14a47a6680117d6b5738
```

A fresh branch database was then extracted from a clean 1,544-target CPU build
configured with `-Denable_cuda=false -Denable_sycl=false`. Replaying the same
query produced **zero results in all selected paths**: `pdjson.c`,
`thread_pool.c`, `test_fex_ctx_vector.cpp`, and
`test_thread_pool_backpressure.c`. The five remaining repository-wide query rows
were outside this alert lane: the pre-existing `dump_c_values` row and a volatile
duplicate-identity group in `predict.c`. This is deliberately not reported as a
repository-wide zero.

Focused runtime verification after the identity repair:

```text
test_pdjson                              PASS
test_pdjson_stack_increment_zero         PASS
test_pdjson_stack_increment_oversized    PASS
test_thread_pool_backpressure             PASS
test_fex_ctx_vector                       PASS
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
- The unrelated `predict.c` duplicate-identity group belongs to a separate audit;
  it is recorded here so this lane does not overclaim a repository-wide zero.

## Related

- [ADR-1142](../adr/1142-whole-codebase-standards.md): whole-tree warning policy.
- `T-CODEQL-UNUSED-STATIC-FUNCTIONS-2026-09-24` in
  [`docs/state.md`](../state.md).
