<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1265: The clang-tidy header filter matches absolute paths, so headers count

- **Status**: Proposed
- **Date**: 2026-09-19
- **Deciders**: Lusoris
- **Tags**: ci, clang-tidy, lint, ratchet, fork-local

## Context

ADR-1142 puts the whole tree under the clang-tidy ratchet (`scripts/ci/tidy-ratchet.py` against
`scripts/ci/tidy-baseline-<lane>.json`, required check `Tidy Ratchet`). `.clang-tidy` names the
headers that count with:

```yaml
HeaderFilterRegex: '^(core/(include|src|tools|test)|python|ai)/.*\.(h|hpp|hxx|cuh)$'
```

clang-tidy tests that regex against the header's path **as it saw it**, which for a
`compile_commands.json` build is absolute — `/home/…/vmafx/core/src/picture.h`. An expression
anchored at `^core/` can never match an absolute path, so every in-repo header has been filtered
out as "non-user code" since the file was written. Measured on one translation unit
(`core/src/picture.c`):

```text
Suppressed 540 warnings (480 in non-user code, 60 NOLINT).
Use -header-filter=.* or leave it as default to display errors from all non-system headers.
```

480 header diagnostics dropped for a single TU. Across the CPU lane the ratchet baseline holds
462 warnings in 72 files and **zero of them are in a header**, although the tree has 60 headers
carrying 313 — `core/src/feature/adm_tools.h` alone has 142. The ratchet has never seen a header,
so ADR-1142's "whole tree" claim has been false for exactly the files that are included everywhere.
Bug ledger `L-45`, reported by the CAMBI agent and confirmed by measurement.

## Decision

The filter becomes `(^|/)(core/(include|src|tools|test)|python|ai)/.*\.(h|hpp|hxx|cuh)$`, which
matches the same set of files whether clang-tidy presents them relative or absolute, and nothing
outside the repository (a system header never contains `/core/src/`). Every lane's baseline is
re-recorded to include the header findings that were always there. The CPU baseline is the one
CI measures (clang-tidy 22 on `ubuntu-26.04`, uploaded as the `tidy-ratchet-cpu` artifact), so it
is committed from that artifact rather than from a local run with a different clang-tidy.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| `HeaderFilterRegex: '.*'` with `SystemHeaders: false` | Simplest | Also admits headers under `subprojects/`, `build/` and the vendored trees the existing pattern deliberately names | Loses the scoping the current regex encodes |
| Pass `--header-filter` from `tidy-ratchet.py` instead of `.clang-tidy` | The script knows the repo root and could build an absolute regex | Every other clang-tidy invocation (IDE, `make lint`, `scripts/dev/preflight.sh`) keeps the broken filter | The file is the single source; fix it there |
| Keep headers excluded and record that as policy | No baseline growth | Contradicts ADR-1142 in writing, and the 313 findings exist whether or not the gate reads them | Would be choosing not to know |
| Fix the regex but keep the old baseline, letting the ratchet fail | Forces immediate cleanup | 313 findings across 60 headers, most in upstream-mirror `adm_tools.h`; blocking every PR on that is not a plan | The ratchet's own rule is "never raise the baseline"; a first *recording* of a class is not a raise |

## Consequences

- **Positive**: the ratchet covers headers. A PR that adds a finding to `adm_tools.h` now fails
  the gate instead of passing silently; a PR that cleans a header must tighten the baseline.
- **Negative**: the CPU baseline grows from 462 to roughly 744 entries, and the tree's real debt
  is visible for the first time. The touched-file rule applies to headers too from here on.
- **Neutral / follow-ups**: GPU-lane baselines are re-recorded from a local run where a toolchain
  exists; those lanes are advisory, not required. `adm_tools.h` (142) is the obvious first cleanup.

## References

- `.clang-tidy`, `scripts/ci/tidy-ratchet.py`, `.github/workflows/lint-and-format.yml` (Tidy Ratchet).
- [ADR-1142](1142-whole-codebase-standards.md), [ADR-0141](0141-touched-file-cleanup-rule.md).
- Bug ledger `L-45`; measurement 2026-09-19 (`clang-tidy -p build --quiet core/src/picture.c`).
- Source: `Q` — popup answer 2026-09-19, "Fix it, re-record all baselines".
