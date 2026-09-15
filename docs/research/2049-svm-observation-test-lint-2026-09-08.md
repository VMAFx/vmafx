# Research-2049: Preserve observation-only SVM tests while cleaning native lint

At `2143174c7bf933f97aec610f423c559a885b8e9a`, exhaustive Cppcheck reports nine
read-only model pointers in `test_svm_parser.c` and five read-only query arrays
in `test_svm_api.c`. Fresh clang-tidy 22.1.8 also reports eleven parser warnings
and seventeen API warnings: C null spelling plus the parser runner's eighteen
branches. The runner exceeds the fifteen-branch limit because each
`mu_run_test` expands to two branches.

The parser's model views and API query arrays are now const. Header-size and
header-order cases move into two small driver helpers, preserving the original
case order and first-error return. Both C files gain the already-required
ADR-1138/ADR-1166 `modernize-use-nullptr` bracket; `NULL` remains compatible with
MSVC's C frontend. No new diagnostic exemption or language policy is introduced.

## Alternatives considered

| Choice | Result |
| --- | --- |
| Const local views and named parser groups | Selected: remove the findings without changing public calls or test behavior. |
| Rewrite the vendored parser or its public types | Rejected: these tests are observation-only under ADR-0952/ADR-0889. |
| Disable the branch limit or replace C `NULL` with `nullptr` | Rejected: weakens existing lint or violates the established MSVC C portability rule. |

## Validation and limits

An independent private CPU build uses GCC 15.2, release optimization, assembly
and default LTO. The original and changed binaries both pass the nine parser
and nineteen runtime API tests. Full stdout/stderr agree byte-for-byte.
A C-token comparison preserves all fifty assertion invocations, twenty-eight
ordered test registrations, sixty-five public SVM calls and every string
literal. `svm.cpp`, `svm.h` and Meson registration remain byte-identical.

The API executable's `.text` and `.rodata` sections are byte-identical. Parser
sections differ after grouping, so no binary-identity claim is made for it.
A separate retained runner control replaces only the public parser entry point
with a stub: the all-pass case and each of nine injected failure positions
produce identical old/new output, first-error return and test count. This
checks driver propagation; the ordinary native runs above use the real library.

Actual scoped clang-tidy measures both touched TUs at zero warnings, compiler
failures and uncited annotations. The guarded writer removes only their
existing allowances, retains unselected entries and full-lane metadata,
recomputes debt totals and appends scoped provenance. Exhaustive Cppcheck with
the official POSIX model and both actual test-driver compile variants exits
zero. No tests, assertion values, model inputs, ownership rules or registrations
are added or removed.

Reproduce with `meson test -C build --print-errorlogs test_svm_parser test_svm_api`.
Exact commands, source/tool hashes, original binaries, section comparisons,
runner controls and hook results are retained locally under
`.workingdir2/evidence/svm-tests-native-lint-2026-09-08/`.
This is test-only validation: no complete RC1, Netflix Python golden,
Windows/GPU or sanitizer acceptance is claimed. There is no user-discoverable
surface or FFmpeg integration change, so no user-guide update is required.

## References

- [ADR-0889: vendored libsvm cordon](../adr/0889-libsvm-vendored-audit.md)
- [Core source invariants](../../core/src/AGENTS.md)
- [C test invariants](../../core/test/AGENTS.md)
- req: "get the codebase clean" while preserving intentional test coverage.
