# Research-2053: Read-only IQA and motion coverage fixtures

At `fb79390160fdfc3f8316dd5ab6cae4c05de2b58d`, exhaustive Cppcheck reports seven
read-only image arrays in `test_iqa_convolve_coverage.c` and five source arrays
in `test_integer_motion_edge16_coverage.c`. Fresh strict clang-tidy adds nineteen
IQA and six motion warnings: C `NULL` spelling and the IQA runner's eighteen
branches, exceeding the fifteen-branch limit.

Twelve input arrays become const. The writable `iqa_img_filter` inputs and
kernel arrays retain their existing types and ownership. The first five IQA
boundary tests move into `run_boundary_tests`; the runner immediately returns
its failure, then executes the unchanged four filter tests. The helper does
not register a new case or increment the count. Both C files use the mandatory
ADR-1138/ADR-1166 `NULL` portability bracket. Production sources and headers
remain unchanged.

## Alternatives considered

| Choice | Result |
| --- | --- |
| Const fixture inputs plus one named boundary-test group | Selected: preserves inputs, assertions, call order and first-failure behavior. |
| Change IQA/motion production types or replace fixtures | Rejected: outside this observation-only cleanup and unnecessary. |
| Suppress the size/const rules or use C `nullptr` | Rejected: violates existing lint/MSVC portability policy. |

## Validation and limits

All fourteen existing cases pass before and after in an independent GCC 15.2
CPU release build with assembly and default LTO. Complete native stdout/stderr
match. All twenty-five assertions, fourteen ordered registrations, twenty-four
IQA/motion API calls and every literal are preserved. Every test-body token
matches after removing only the twelve added const qualifiers.

Both actual release/LTO executables have byte-identical `.text*`/`.rodata*`
sections: 2,846 bytes for IQA and 932 bytes for motion. The diagnostic motion
object also matches (544 bytes); these diagnostic objects use the native
command with LTO disabled solely to materialize code. The IQA diagnostic
object differs after grouping and is retained without an identity claim.
The actual release executable identity also preserves its failure branches.

Strict clang-tidy 22.1.8 measures both touched TUs at zero warnings, uncited
annotations and compile failures. The guarded scoped writer removes their
nineteen/six warning allowances, preserves unselected entries/full-lane metadata,
recomputes totals and appends scoped provenance. Exhaustive Cppcheck with the
actual configured test drivers and official POSIX model exits zero.

Reproduce:

```bash
meson test -C build --print-errorlogs \
  test_iqa_convolve_coverage test_integer_motion_edge16_coverage
```

Exact source, native flags, original/changed objects and binaries, token/section
comparisons and hook receipts are retained locally under
`.workingdir2/evidence/observation-fixture-const-2026-09-08/`.
No new tests, public surfaces, FFmpeg integration or golden assertion changes.
No user guide is needed for this test-only cleanup. Full RC1, Windows/GPU,
Netflix Python golden and sanitizer acceptance remain separate.

## References

- [C test invariants](../../core/test/AGENTS.md)
- [Core source invariants](../../core/src/AGENTS.md)
- req: "get the codebase clean" while preserving meaningful coverage.
