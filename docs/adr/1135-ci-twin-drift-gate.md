<!-- markdownlint-disable MD013 -->
# ADR-1135: CI twin-drift + stale-source-reference gate

- **Status**: Accepted
- **Date**: 2026-09-02
- **Deciders**: Lusoris
- **Tags**: ci, build, process, fork-local, claude-rule

## Context

The C++23 migration ([ADR-0729](0729-cpp23-wave3-bundle.md) and its
sibling waves, wired into the build by #1133) left `core/` with ten
same-directory `.c`/`.cpp` twin pairs. libvmaf compiles the `.cpp` side of
`gpu_picture_pool`, `log`, `opt`, `picture_pool` and `read_json_model`,
`core/tools` compiles `cli_parse.cpp`, and libvmaf compiles the `.c` side
of `feature_collector`. The other side of each pair is compiled only by
`core/test/meson.build` or `core/test/fuzz/meson.build`, and three sides —
`core/src/model.cpp`, `core/test/test_dict.c`, `core/test/test_feature.c` —
are compiled by nothing at all.

Twice this year the split bit. `output.c` and `output.cpp` diverged for
three months because a fix landed on one twin only. The `mem.c → mem.cpp`
and `dict.c → dict.cpp` renames left stale paths in the Cython `.pyx` +
`python/setup.py` (T-CYTHON-MEM-CPP-2026-08-30, fixed in #1161) and in
`core/test/fuzz/meson.build` (T-FUZZ-DICT-CPP-STALE-2026-08-31, #1186);
both builds stayed broken for two months because nothing on the PR path
configures the fuzz harnesses or builds the Cython extension, and nothing
checked the references statically. The planning dossier recommended a CI
gate after the third recurrence.

Both predicates — "is every twin side compiled by something?" and "does
every source path a build file names exist?" — are mechanically decidable
from `git ls-files` plus the build files. That is the bar
[ADR-0334](0334-state-md-touch-check-ci-gate.md) set for turning a
reviewer-enforced rule into a blocking check.

## Decision

We add a blocking, **required** CI check
`Twin Drift + Stale Source Refs (ADR-1135)` — job `twin-drift-check` in
[`lint-and-format.yml`](../../.github/workflows/lint-and-format.yml),
listed verbatim in
[`required-aggregator.yml`](../../.github/workflows/required-aggregator.yml)
— backed by
[`scripts/ci/twin-drift-check.sh`](../../scripts/ci/twin-drift-check.sh).
It fails when:

- **(a)** a same-directory `.c`/`.cpp` twin pair exists whose `.c` or
  `.cpp` side is referenced by **no** build file (`meson.build`,
  `setup.py`, `*.pyx`), unless that side is listed in
  [`scripts/ci/twin-drift-allowlist.txt`](../../scripts/ci/twin-drift-allowlist.txt)
  **with a reason**;
- **(b)** any build file references a source-file literal (extensions
  `c cpp cc cxx cu hip m mm metal pyx`) that does not resolve to a file in
  the tree.

Resolution rules: literals resolve relative to the build file's directory;
`var + 'x.c'` resolves through `var = './dir/'` assignments in the same
file; `os.path.join(...)` is joined, with identifiers resolved through the
same assignment table; `output:` arguments and `@PLAINNAME@`-style
substitutions are generated files and are skipped; absolute paths are
skipped; a prefix that is not a literal directory (loop variables such as
`_m + '_parity.c'`) falls back to a suffix search over the tracked-file
list and is reported as `NOTE`. Comments are stripped quote-aware before
extraction. Headers are out of scope (see alternatives).

The allowlist is validated, not trusted: a row without a reason, a row
whose file is gone, whose side is compiled again, or whose pair no longer
exists fails the gate, so the file cannot rot. There is **no** allowlist
for (b): a stale reference is fixed, never listed. The only per-line escape
hatch is an inline `twin-drift-ignore: <reason>` comment (reason
mandatory, in the spirit of [ADR-0278](0278-t7-5-nolint-sweep.md)) for a
construct the parser cannot model.

Sides referenced only by test / fuzz build files are printed as `INFO`
(non-failing) so the drift-risk shape the dossier tracks stays visible on
every run. The same script runs locally as a `pre-push` pre-commit hook and
is exercised by 24 fixture cases in
[`scripts/ci/tests/test-twin-drift-check.sh`](../../scripts/ci/tests/test-twin-drift-check.sh).

On the pristine master tree the gate reports exactly one finding — the
fuzz `dict.c` reference — and zero false positives across 1233 resolved
literals; the same one-line fix as #1186 lands in the introducing PR so the
check is green from its first commit.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Configure every meson target (fuzz, Cython, DNN tests) on every PR** | Catches stale paths by construction | Fuzz needs clang + libFuzzer, Cython needs a wheel build; minutes per PR, and it says nothing about dead twins | Rejected — (b) is a two-second static check and (a) is not a build failure at all. |
| **`meson introspect --targets` instead of parsing `meson.build`** | Exact semantics, no regexes | Needs a successful `meson setup` per option combination; CUDA / SYCL / HIP / Metal / fuzz sources sit behind `if` blocks, so one configuration sees a fraction of the literals | Rejected — the static parser sees every branch; on master all 1233 literals resolve exactly with one suffix-search fallback. |
| **Python implementation under `scripts/ci/`** | Easier unit testing, richer parsing | Deviates from the `assertion-density.sh` / `state-md-touch-check.sh` shape; a runtime dependency for a git-plus-text predicate | Rejected — bash + POSIX awk mirrors the sibling gates; output verified identical under `gawk --posix` and `--traditional` (mawk is Ubuntu's default awk). |
| **Delete the three dead twins instead of allowlisting them** | Empty allowlist from day one | `model.cpp` is a real ADR-0729 twin waiting to be wired in; the two orphan test `.c` files carry the Netflix header and their removal deserves its own reviewable PR | Deferred — allowlisted with the follow-up named per row (T-TWIN-DEAD-SIDES-2026-09-02). |
| **Advisory first, promote after a false-positive audit (ADR-0334 pattern)** | Reversible if the parser over-fires | No PR-body heuristics are involved; every literal on master was verified to resolve; the historical failure mode is exactly "nobody looked at the nightly red" | Rejected — required from the first commit per operator direction; the validated allowlist and per-line ignore are the escape hatches. |
| **Include header literals (`.h`, `.hpp`)** | Would catch stale `install_headers()` rows | `cc.has_header('unistd.h')` and `prefix: '#include <…>'` name system headers that never exist in-tree; needs a per-call-site context model | Deferred — the extension list is one regex; widen it when a header rename actually bites. |

## Consequences

- **Positive**: the two failure classes that cost three and two months
  respectively now fail the PR that introduces them, in about two seconds,
  naming the build file and line. The allowlist doubles as a living
  inventory of dead twins with a stated reason and follow-up per row.
  `INFO` lines keep the test-only twin sides visible.
- **Negative**: one more required check on every PR. A new build-file
  idiom the parser does not model (for example a multi-line `output:` list
  of `.c` files) may need the per-line ignore marker until the parser and
  its fixture harness learn it. Headers are not covered.
- **Neutral / follow-ups**: T-TWIN-DEAD-SIDES-2026-09-02 (wire in or
  delete `model.cpp`; delete `test_dict.c` / `test_feature.c`) shrinks the
  allowlist to zero. Unity `#include "foo.c"` references inside sources are
  a candidate extension of predicate (b). Renaming the workflow job renames
  the aggregator row in the same commit
  ([ADR-0313](0313-ci-required-checks-aggregator.md) matches names
  exactly).

## References

- [ADR-0729](0729-cpp23-wave3-bundle.md) — the C++23 wave that created
  the twin pairs; [ADR-0761](0761-cpp23-wave8-opt-read-json-model.md) and
  [ADR-0768](0768-cpp23-wave9-pool-env.md) — the `*_cpp23_lib` wiring
  pattern the live `.cpp` sides use.
- [ADR-0313](0313-ci-required-checks-aggregator.md) — required-check
  aggregation; job names are matched verbatim.
- [ADR-0334](0334-state-md-touch-check-ci-gate.md) — precedent for
  script-plus-thin-workflow blocking gates with a fixture harness.
- [ADR-0278](0278-t7-5-nolint-sweep.md) — every suppression cites its
  reason inline; the allowlist and ignore marker follow the same rule.
- [ADR-0270](0270-fuzzing-scaffold.md) — the fuzz harnesses whose stale
  `dict.c` reference the gate caught on its first run.
- Related PRs: #1133 (twins wired), #1161 (Cython `mem.c` fix), #1186
  (fuzz `dict.c` fix).
- [`docs/development/ci.md`](../development/ci.md#twin-drift-gate) —
  contributor-facing documentation, added in the same PR.
- Source: `req` — operator direction 2026-09-02 (paraphrased): add the CI
  gate the planning dossier recommended after the third twin-drift
  recurrence; fail on a twin side no build file compiles and on any build
  file naming a source that does not exist; allowlist the current dead
  twins explicitly, each with a reason.
