<!-- markdownlint-disable MD013 MD060 -->
# ADR-1230: The CI gcc moves forward with clang and meson, and the ratchet records it

- **Status**: Accepted
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: ci, build, tooling, clang-tidy, fork-local

## Context

The clang-tidy lanes in `lint-and-format.yml` install three toolchain
components, and until now treated them inconsistently:

| Tool | `ubuntu-24.04` ships | What CI did | Rationale in the workflow |
| --- | --- | --- | --- |
| clang | 18 — cannot parse the tree's C++26 `std::expected` | installs **clang-22** from `apt.llvm.org` | yes, in a comment |
| meson | 1.3.2 — predates `c23` in `c_std` | installs from **PyPI** | yes, in a comment |
| **gcc** | **14** | **used gcc-14** | **none** |

Two of the three are deliberately pulled forward past the distro because the
tree targets C23 / C++26 (ADR-0692). gcc was left at whatever the runner image
happened to ship. No ADR pins it; the only mention of `gcc-14` anywhere in
`docs/adr/` is an incidental line in a VVenC changelog summary.

That is not free, and it stopped being theoretical on PR #1392. The ratchet
reported `core/test/test_pooling_percentile.c: warnings 0 -> 1 (+1)` while
`Tidy Changed` — which fails on *any* warning and does not exclude that file —
reported the same file clean, on the same runner and the same commit. The
delta could not be reproduced on gcc-15 or gcc-16; on both, the only warning
was a glibc system-header diagnostic the ratchet correctly discards.

The cause is structural. clang-tidy parses each translation unit against the
system headers the **C compiler** provides, so a ratchet count depends on gcc's
version as much as on clang-tidy's — and the baseline recorded only
`clang_tidy_version`. Pinning gcc a major behind every developer's machine,
while recording nothing about it, makes a count that disagrees with local
measurement impossible to explain.

The report artifact made it worse: `tidy-ratchet.py` parses every diagnostic in
`parse_diagnostics()` and then discards everything but the per-file count, so
"which warning?" had no answer anywhere in CI's output.

## Decision

**1. gcc moves forward with the rest of the toolchain.** The clang-tidy lanes
install `gcc-15` / `g++-15` from `ppa:ubuntu-toolchain-r/test`, the same
"pull the toolchain past the distro" treatment clang and meson already get,
for the same reason.

**2. The ratchet records the compiler it measured with.** `cc_version` joins
`clang_tidy_version` in the baseline and the report, read from the build
directory's `meson-info/intro-compilers.json` rather than from `$CC`, so the
recorded value is the compiler that actually produced
`compile_commands.json`. A mismatch against the baseline annotates a warning,
exactly as a clang-tidy mismatch already does.

**3. The report artifact keeps the diagnostics.** Every measured diagnostic is
recorded as `path:line:col: [check]` in the `--report` JSON. The **baseline**
deliberately does not carry them: it stays a reviewable count file that does
not churn every time a line number shifts. The report is where you look when a
delta needs explaining.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Leave gcc at 14 | Zero churn; the baseline stays valid as-is | Keeps the tree's build compiler a major behind its own C23/C++26 target, and keeps every ratchet delta unreproducible off the CI image — the defect this ADR exists to fix | Rejected |
| Move the lanes to an `ubuntu-26.04` runner image | Newer gcc with no PPA | GitHub's hosted 26.04 image availability is not something this repo should depend on for a required check; the PPA is explicit and version-pinned | Rejected for now; revisit when 26.04 is the default `ubuntu-latest` |
| Bump gcc across `libvmaf-build-matrix.yml` too | Consistency everywhere; the matrix is equally a major behind | A compiler bump across ten-plus matrix legs surfaces its own crop of new warnings and belongs in a PR whose CI failures are about exactly that, not mixed into a diagnostics fix | Deferred to a follow-up, deliberately |
| Record diagnostics in the baseline too | One file to look at | The baseline would churn on every line-number shift and stop being reviewable, and its whole value is being a stable, diffable count | Rejected |
| Keep counts only and reproduce locally when needed | No script change | That is precisely what failed on #1392: local reproduction is impossible when CI's compiler is pinned to a version developers do not have | Rejected |

## Consequences

- **Positive**: the build compiler stops being the one component frozen at the
  distro's version; a future ratchet delta names the check and the line
  instead of only a count; a compiler change is announced rather than silently
  shifting every count.
- **Negative**: **the cpu baseline must be regenerated.** Warning counts under
  gcc-15's headers will not equal gcc-14's, so `scripts/ci/tidy-baseline-cpu.json`
  is stale the moment this merges. It has to be rewritten from a CI run
  (`tidy-ratchet.py --write` on the lane), never from a developer machine —
  a local run resolves `enable_dnn=auto` differently and would rebaseline
  unrelated files against the wrong build.
- **Neutral / follow-ups**: `libvmaf-build-matrix.yml` still pins `gcc-14`
  across its legs; moving it is a separate PR. The `cc_version` field is
  optional on read, so an existing baseline without it loads unchanged and
  simply does not trigger the mismatch warning until it is regenerated.

## References

- req: the user asked why CI was going backwards on gcc when the project's
  direction is newer toolchains.
- PR #1392 — the unreproducible `+1` that exposed both halves of this.
- [ADR-0692](0692-vmafx-c23-bump.md) — the C23 / C++26 target that justifies
  pulling clang and meson past the distro, and now gcc with them.
- [ADR-1142](1142-whole-codebase-standards.md) — the ratchet this makes
  diagnosable.
