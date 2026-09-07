<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1222: In-code suppressions do not close code-scanning alerts; scope the scan instead

- **Status**: Proposed
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: `ci`, `security`, `docs`, `mcp`

## Context

The repository's Security tab carries 20 open alerts that never clear, even
though several of the flagged sites already carry in-code justifications. The
scanners are **not** disabled: `security-scans.yml` is `active`, CodeQL analysed
`refs/heads/master` most recently on 2026-09-07 with 57 results, and Semgrep
uploads both its `semgrep-local` and `semgrep-registry` SARIF categories on
every master push. The alerts persist for two mechanical reasons that no amount
of in-code annotation can fix.

**1. `# nosemgrep` does not remove a result from the SARIF.** Semgrep's docs are
explicit that a `nosemgrep` comment "still generates findings records that are
automatically set to **Ignored** triage state, rather than excluding code from
scanning entirely." That triage state lives on the Semgrep platform. GitHub code
scanning ingests the SARIF, sees a result, and keeps the alert open. Three SHA-1
alerts in `compat/python-vmaf/tools/decorator.py` and one file-permission alert
in `ai/sidecar/online_trainer.py` are in exactly this state: the code is already
correct (`hashlib.sha1(..., usedforsecurity=False)` for a memoisation cache key;
`0o660` on a Unix-domain socket shared with a same-group Go peer), each carries
a justification comment and a `nosemgrep` directive, and the alerts stay open.

**2. `paths-ignore` is inert for compiled languages that are built.** GitHub's
documentation limits `paths` / `paths-ignore` to interpreted languages and to
compiled languages analysed *without* building. This repo builds C/C++ with
meson + ninja in the CodeQL job, so every translation unit the build compiles is
extracted and analysed regardless of the config. `core/test` is listed in
`paths-ignore` and still produces alerts; so does meson's generated probe file
under `core/build/meson-private/`. A second, independent defect compounded it:
the entry was a bare `build`, and GitHub's glob rules make that match only a
**top-level** `build` directory — the fork's build trees are `core/build`,
`core/build-cuda`, and so on.

Separately, the `cpp/unused-local-variable` alert on `core/tools/cli_parse.cpp`
flags a variadic-template parameter pack that *is* used
(`std::forward<Rest>(rest)...`), and `cpp/commented-out-code` flags a block
comment that is prose, not code. Both are query false positives.

## Decision

We will treat the scan's **scope** as the thing to fix, not the code:

- Correct `paths-ignore` to `**/build`, `**/build-*`, `**/builddir` so the globs
  match at any depth, and record inline that the whole list is inert for the
  built `cpp` analysis — with the doc link — so the next reader does not add
  entries expecting them to work.
- Remove genuinely dead code the scanners found: `mcp-server`'s duplicated
  `_VALID_*` constant block.
- Leave every remaining alert open and **document each one's disposition here**
  rather than dismissing it. Dismissal is the maintainer's call, not an agent's.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Fix the scope, document the rest (chosen) | Removes the two mechanical causes; no alert is silently hidden; the maintainer keeps the dismissal decision | The Security tab still shows justified findings until someone dismisses them | — |
| Dismiss the justified alerts via the API | Empty Security tab | The standing project rule is that agents analyse and fix while the maintainer decides dismissals; an agent dismissing a `high` CodeQL alert is exactly the failure that rule exists to prevent | Violates the rule |
| Post-process the SARIF to drop allowlisted results before upload | Alerts would actually clear; the allowlist is in-tree and reviewable | It is a dismissal mechanism wearing different clothes, and it hides future *new* instances of the same rule in the same file | Proposed for the maintainer, not adopted unilaterally |
| Drop `security-and-quality` and keep only `security-extended` | Removes all 13 note-level alerts at a stroke | Also removes real quality signal, and the notes are not the problem — the two mechanical causes are | Throws away signal to fix a symptom |
| Stop building `core/test` in the CodeQL job | Would genuinely exclude the test-file alerts | The tests are fork-added C the whole-tree lint policy (ADR-1142) deliberately covers; excluding them from CodeQL contradicts that | Contradicts ADR-1142 |

## Consequences

- **Positive**: the two causes that made alerts unclearable are gone. Meson's
  generated probe files stop being analysed. The `mcp-server` constant block no
  longer has two definitions of five names, where editing the first was a
  silent no-op.
- **Negative**: the Security tab still shows justified findings. The disposition
  table below is the record of why.
- **Neutral / follow-ups**: whether to adopt SARIF post-processing, and whether
  to dismiss the alerts listed as "correct as written", are decisions left to
  the maintainer.

### Alert disposition at the time of writing

| Alert | Rule | Location | Disposition |
| --- | --- | --- | --- |
| 1005 | `cpp/integer-multiplication-cast-to-long` (**high**) | `core/src/feature/iqa/convolve.c:155` | **Analysed, deliberately not fixed.** Widening the multiply desynchronises the AVX2/AVX-512/NEON twins from the scalar path and breaks the ADR-0138 bit-exactness invariant (`test_iqa_convolve` fails). The overflow is unreachable: image samples times a normalised kernel bound the product at 255 in magnitude. Already recorded in `docs/state.md` as `T-CODEQL-FLOAT-WIDEN-MULT-2026-09-06`. |
| 970, 971 | `py/unused-global-variable` | `mcp-server/.../server.py:373-374` | **Fixed** — dead duplicate block removed. |
| 1018 | `cpp/unused-local-variable` | `core/build/meson-private/.../testfile.c` | **Fixed by scope** — a meson-generated probe file; `paths-ignore` now matches `**/build`. |
| 1002, 1003 | `cpp/unused-local-variable`, `cpp/unused-static-variable` | `core/tools/cli_parse.cpp:321` | **False positive.** `rest` is a template parameter pack expanded at `std::forward<Rest>(rest)...`; the query does not model pack expansion. |
| 951 | `cpp/commented-out-code` | `core/test/test_model_feature_overload_ownership.c:183` | **False positive.** The block is prose explaining the test's design, not code. |
| 168, 927 | `cpp/equality-on-floats` | `feature_name.cpp:146`, `predict.c:301` | **Correct as written.** Both are exact identity comparisons by design — "is this value the declared default?" and a sentinel check that already carries an inline justification. An epsilon band would be the bug. |
| 908, 943, 955 | `cpp/include-non-header` | three files under `core/test/` | **Correct as written.** Whitebox unit tests include the translation unit under test to reach `static` functions. `paths-ignore: core/test` cannot exclude them because the C/C++ analysis builds them. |
| 917, 918 | `py/cyclic-import` | `server.py`, `http_transport.py` | **Correct as written.** Both sides already use function-local imports, the standard way to break a runtime cycle; the static import graph still contains one. |
| 946 | `insecure-file-permissions` | `ai/sidecar/online_trainer.py:430` | **Correct as written.** `0o660` on a Unix-domain socket shared with a same-group Go peer; world access is denied. Carries a `nosemgrep` directive that cannot close the alert (cause 1 above). |
| 947, 948, 949 | `insecure-hash-algorithm-sha1` | `compat/python-vmaf/tools/decorator.py` | **Correct as written.** SHA-1 as a memoisation cache key, already annotated `usedforsecurity=False`. Same `nosemgrep` limitation. |
| 1, 3 | Scorecard `CodeReviewID`, `CIIBestPracticesID` | repo-level | **Not code.** Repo-process metrics (approved-changeset count; OpenSSF badge). No code change clears them. |

## References

- [Customizing your advanced setup for code scanning](https://docs.github.com/en/code-security/code-scanning/creating-an-advanced-setup-for-code-scanning/customizing-your-advanced-setup-for-code-scanning)
  — `paths` / `paths-ignore` apply to interpreted languages and to compiled
  languages analysed without building; glob rules (`build` matches only a
  top-level directory, `**` must be its own path segment).
- [Semgrep: ignoring files, folders, code](https://docs.semgrep.dev/ignoring-files-folders-code)
  — `nosemgrep` sets an Ignored triage state; it does not remove the finding.
- [ADR-0348](0348-codeql-poorly-documented-function-suppressed.md) — the existing `query-filters`
  suppression of `cpp/poorly-documented-function` and its rationale.
- [ADR-1142](1142-whole-codebase-standards.md) — the whole-tree lint policy
  that keeps `core/test` in scope.
