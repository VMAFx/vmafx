<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3355: CodeQL analyses the C/C++ build as C++23

- **Status**: Accepted
- **Date**: 2026-10-19
- **Deciders**: lusoris
- **Tags**: ci, security

## Context

`core/meson.build` asks for `cpp_std=c++26,c++23,c++latest`, so the C/C++ build
of the `codeql-cpp` job in `.github/workflows/security-scans.yml` compiles every
C++ translation unit with `-std=c++26`. On 2026-10-19 CodeQL reported 16 new
note alerts in `core/tools/cli_parse.cpp` and `core/tools/cli_options.gen.inc`:
six `cpp/local-variable-hides-global-variable` (lines 436 to 504) and ten
`cpp/unused-static-variable` (lines 179 to 789, and lines 14 and 71 of the
generated include). The file has no global variable that the locals could hide,
and the flagged statics are used.

A local run of CodeQL 2.27.1 on the two compiles of `cli_parse.cpp` shows the
cause. The extractor records `-std=c++26` as `std_version` 201703 (C++17),
reports parse errors in the GCC standard library headers and, from the
misparsed declarations, builds a global variable `settings` at
`cli_parse.cpp:1401` that the source does not have. With `-std=c++23` or
`-std=c++20` (`std_version` 202302 and 202002) the extractor reports no parse
error and none of the 16 alerts. CodeQL's list of supported language versions
ends at C++23.

## Decision

The `codeql-cpp` job configures its build with `-Dcpp_std=c++23`, the second
entry of `core/meson.build`'s list and the newest standard the extractor
parses. The product build keeps `c++26`. `scripts/ci/test_security_workflow_contract.py`
fails when the job's `meson setup` sets another standard or none.

## Alternatives considered

| Option | For | Against | Verdict |
| --- | --- | --- | --- |
| Pin the CodeQL build to `c++23` | Removes the cause; the analysed code is the same source, and the tree builds warning-free as C++23 | The analysed build is not byte for byte the shipped build | Chosen |
| Dismiss the 16 alerts as false positives | No workflow change | Needs a ruling per alert; every later C++ file can raise the same class again, and real findings hide behind parse errors | Rejected |
| Rewrite `cli_parse.cpp` until the alerts go away | Keeps `c++26` | The code is correct; the extractor misreads the standard headers, so any file that includes them stays at risk | Rejected |
| Wait for a CodeQL release that parses C++26 | No local change | No such release is announced; the alerts stay open until then | Rejected |
| Pin `c++20` | Also parses cleanly | Older than needed; C++23 code in the tree would no longer build | Rejected |

## Consequences

- The 16 alerts close as fixed on the first master analysis after this change.
- When CodeQL lists C++26 as supported, the pin and the contract test move to
  `c++26` in one change.
- Code that needs C++26 and does not build as C++23 fails the `codeql-cpp` job;
  that is the signal to revisit this decision.

## References

- Operator order `vmafx-codeql-notes-20261019` (paraphrased): fix the 16 new CodeQL note alerts in one pull request, or close each with a reason.
- [ADR-3294](3294-codeql-every-language-every-pr.md): CodeQL analyses every language on every event.
- CodeQL supported languages and frameworks (C/C++ versions up to C++23).
