<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1762: Every translation unit is read by a clang-tidy lane or excepted by name

- **Status**: Accepted (status update 2026-10-06 below)
- **Date**: 2026-10-05
- **Deciders**: Lusoris
- **Tags**: ci, clang-tidy, lint, ratchet, metal, fuzz, mcp, fork-local

## Context

[ADR-1142](1142-whole-codebase-standards.md) applies clang-tidy to the whole
tree through per-lane baselines, but a baseline only counts the translation
units its lane compiles. An audit of master found 107 of 759 tracked
translation units in no lane's `measured_sources`: the embedded MCP server and
its tests (meson builds them only with `-Denable_mcp=true`), the libFuzzer
harnesses (`-Dfuzz=true` is a configure error under gcc), the Objective-C++
Metal host code and Metal-only C tests (they need Apple's SDK), the Metal
kernels, the HISS rule fixtures, an eBPF program, Windows-only files, the Rust
glue, the Pelorus mirror and two standalone probes. Nothing failed when a unit
was in no lane, so the set could only grow.

## Decision

Every tracked `.c`, `.cc`, `.cpp`, `.cxx`, `.cu`, `.hip`, `.mm` and `.metal`
file is in the `measured_sources` of at least one
`scripts/ci/tidy-baseline-<lane>.json`, or in
the shared lint exception list (`.config/lint-exceptions.d/`) with one path,
the rule `clang-tidy-coverage`, a reason naming the missing tool or toolchain,
and an expiry date. A check (a pre-commit hook, run by CI on every pull
request; it lands in the follow-up pull request) fails on an unread unit, an expired entry, an entry for a
missing file and an entry for a file a lane reads. The readable files join a
lane: the `cpu` lane configures the MCP server; a new `clang` lane builds the
fuzz harnesses and measures only them; a macOS `metal` lane (Homebrew
`llvm@22`, the Xcode SDK, workflow `tidy-metal.yml`) measures the Metal host
code. Lanes that read one part of the tree take `--select`; a scoped baseline
write records the units it measures. Upstream clang-tidy has no Metal language
mode (`clang -x metal` answers "language not recognized"), so the 17 kernels
are listed with that reason.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Gate on the coverage gap (chosen) | A new unit cannot enter the tree unread; the debt is bounded by dated entries | One more list to keep | |
| Leave the gap as a recorded state row | No tooling | The set only grows; the audit found it by accident | A gap with no gate is the failure this decision ends |
| One big lane that builds everything | One baseline | gcc and clang, CUDA and SYCL, Linux and macOS cannot configure together; a failed configure hides every file | The unit of measurement is a configuration the toolchain can build |
| Fuzz in the `cpu` lane through a clang build | No extra lane | Moves the hosted `Tidy Ratchet` job, which must stay gcc-15, to clang | The hosted measurement must stay byte-identical to the container's |
| macOS lane as a required per-pull-request check | Strongest gate | A macOS runner bills ten times a Linux one | Weekly, on Metal changes and on dispatch, like the tester bundle (ADR-1595); required once it has passed on master |

## Consequences

- **Positive**: the unread set is bounded and dated; new MCP and fuzz code is
  measured; Metal host code gets a gate of its own.
- **Negative**: a `clang` baseline and a `metal` baseline to maintain; the
  `metal` lane has no container and cannot be written from the workstation.
- **Neutral / follow-ups**: a Rust-enabled lane, a lane with CUDA and SYCL
  together and a Windows lane retire the matching entries before their expiry.

## References

- `Q` (maintainer popup answer, 2026-10-05, "Gated debt first"): before the
  rc.3 tag every C, C++ and Objective-C++ translation unit is read by a
  clang-tidy lane with zero findings, or, only where no tool can read it, is
  listed with a reason.
- [ADR-1142](1142-whole-codebase-standards.md) (the ratchet),
  [ADR-1243](1243-tidy-scoped-baseline-tightening.md) (scoped writes),
  [ADR-1471](1471-tidy-lanes-in-dev-container.md) (container lanes),
  [ADR-1113](1113-vendor-pelorus-interop-abi.md) (the Pelorus mirror).

## Status update 2026-10-06: Accepted

The decision was applied while this record still said Proposed. Checked on
`origin/master` `fd8b8c93b`: #2101 (`483c8274b`, 2026-10-05) added the
coverage check `scripts/ci/check-tidy-coverage.py` and its pre-commit hook
`clang-tidy-coverage`, the shared exception list
`.config/lint-exceptions.d/clang-tidy-coverage.toml`, the `clang` and `metal`
baselines (`scripts/ci/tidy-baseline-clang.json`,
`scripts/ci/tidy-baseline-metal.json`) and the macOS lane
`.github/workflows/tidy-metal.yml`; `docs/development/tidy-lanes.md` describes
the lanes.

The body above is unchanged.
