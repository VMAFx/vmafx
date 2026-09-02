# ADR-1142: Whole-codebase standards; lint debt only ratchets down

- **Status**: Accepted
- **Date**: 2026-09-02
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, process, code-quality, agents, cuda, sycl, hip, metal, simd

## Context

The fork's coding standards ([principles.md](../principles.md): NASA/JPL
Power of 10, SEI CERT C/C++, the `.clang-tidy` profile, cppcheck, the
banned-function list, licence headers, doc-substance) were written to
apply to "fork-added" code. Everything else was carved out one exemption
at a time:

- [ADR-0141](0141-touched-file-cleanup-rule.md) limits per-PR cleanup to
  the files a PR *touches*; untouched files never have to become clean.
- The required `Clang-Tidy (Changed C/C++ Files)` job checks only changed
  files, and even then pipes the list through nineteen `grep -v`
  exclusions (`core/src/feature/arm64/`, `core/src/cuda/`,
  `core/src/feature/cuda/`, `core/src/sycl/`, `core/src/feature/sycl/`,
  `core/src/hip/`, `core/src/feature/hip/`, `core/src/mcp/`,
  `core/test/fuzz/`, `core/src/compat/win32/`, `core/src/interop/pelorus_*`,
  `core/include/libvmaf/pelorus/`, `core/tools/vmaf_vpl.c`, the GPU and
  MCP test files, …). It fails only on the `WarningsAsErrors` subset.
- The SYCL clang-tidy lane is `continue-on-error: true` ("Advisory").
- The nightly "Full clang-tidy scan" uploads a log that nothing gates on.

The result, measured rather than assumed (2026-08-31 CPU build, 2026-09-02
GPU builds with the workstation's nvcc / icpx / hipcc toolchains,
`clang-tidy` 22.1.8, diagnostics deduplicated by path, line, column and
check):

| Tree | Files / TUs | LOC | clang-tidy warnings |
| --- | --- | --- | --- |
| CPU (`core/src`, `core/tools`, `core/test`) | 281 TUs | — | 5,354 |
| CUDA | 76 | 22,759 | 1,650 |
| SYCL | 28 | 18,427 | 716 |
| HIP | 74 | 22,385 | 1,173 |
| Metal (`.mm`/`.metal` by structural proxy) | 46 | 17,542 | 4 |
| **Whole tree** | — | — | **8,897** |

Sixty-eight of the 84 upstream-mirror files under `core/src` and
`core/tools` had never been reworked. None of this is visible in CI: master
reads green while the debt grows, because nothing measures the whole tree
and nothing forbids it from getting worse.

The maintainer's direction on 2026-09-02 (see References) ended the
two-class standard: from now on every rule applies to the whole codebase,
with no restriction by origin, language or backend. The only invariant that
sits *above* the standards is numerical correctness as pinned by the
Netflix golden assertions.

## Decision

We will apply every standard and every gate to **every file in the tree**,
and we will enforce it with a **whole-tree ratchet** instead of a
touched-files rule.

1. **Scope.** The standards in [principles.md](../principles.md) and every
   linter in `make lint` apply regardless of origin (upstream-mirror
   Netflix code, vendored libraries, fork-added code), language (C, C++,
   CUDA, HIP, SYCL, Metal Shading Language and Objective-C++, Python, Go,
   Rust, shell, YAML, Markdown) and backend. There is no "upstream code"
   tier, no "GPU code" tier and no "test code" tier. The Netflix golden
   assertions remain the sole hard invariant: any rework that moves a
   golden score is wrong, however clean it lints.
2. **Ratchet, not sweep.** `scripts/ci/tidy-ratchet.py` measures every
   translation unit of a `compile_commands.json` with clang-tidy,
   deduplicates diagnostics by `(path, line, column, check)`, counts
   `NOLINT` markers that carry no inline `ADR-NNNN` citation, and compares
   the per-file numbers with a committed baseline
   `scripts/ci/tidy-baseline-<lane>.json`. The rule is *baseline equals
   measurement*: a file above its baseline is a regression (exit 2); a file
   below it means the baseline must be tightened in the same PR with
   `--write` (exit 3); a translation unit clang-tidy cannot compile makes
   the measurement unusable and fails closed (exit 4). The baseline can
   therefore only ever go down, file by file, until it is empty.
3. **Lanes.** The `cpu` lane (the CI CPU build) runs on every PR as the
   aggregator-required context `Clang-Tidy Ratchet (Whole Tree)`. The
   `cuda`, `sycl` and `hip` lanes have committed baselines measured with
   the same script on the workstation toolchains; they run in the nightly
   workflow and locally via `make tidy-ratchet LANE=<lane>`, and each one
   becomes a PR-required context the moment a hosted toolchain for it
   exists in CI (SYCL first: the advisory job already installs oneAPI).
   Metal stays on the structural proxy until a macOS lane exists. A lane
   that cannot run is reported as *not run*, never as *clean*.
4. **ADR-0141 is subsumed, not repealed.** A touched file must still end
   the PR at zero warnings (the existing changed-files job keeps that fast
   feedback, and keeps `WarningsAsErrors` as a hard stop). What changes is
   that untouched files are now bounded too, and that the debt is a
   first-class, committed, reviewable number.
5. **Carve-outs need a reason that survives review.** The only exemptions
   allowed are generated files, third-party test fixtures, licence text and
   the Netflix golden assertions themselves. Every other exclusion in the
   lint and CI configuration is retired in the PR that lands this ADR or
   listed in its follow-ups with the toolchain that blocks it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Whole-tree per-file ratchet with a committed baseline, all lanes, GPU lanes nightly until hosted toolchains exist (chosen)** | Debt is measured, visible and monotonic; no big-bang sweep blocks other work; upstream-mirror and GPU code are held to the same bar; per-file granularity stops one file growing behind another's cleanup | Every C PR pays a whole-tree clang-tidy run (about ten minutes on a hosted runner; routed by the ADR-1140 impact planner to C changes only); the baseline file churns with every cleanup | **Decision** |
| Keep ADR-0141 (touched files only) plus the nightly report | Zero new CI cost | The 8,897-warning debt stays invisible and unbounded; two-class standard persists; contradicts the maintainer's direction | Rejected — status quo is what produced the debt |
| Sweep the whole tree to zero first, then turn on a zero-tolerance gate | Clean baseline from day one | 8,897 warnings across 505 files is weeks of rework; blocks every other PR until done; golden-gated rework must be done in reviewable bundles, not one sweep | Rejected — sequence backwards; the ratchet lets the sweep happen in waves |
| Ratchet on the total count only | Simpler baseline (one number) | A file can grow while another shrinks; regressions hide inside the total | Rejected — the file is the unit reviewers reason about |
| Ratchet without the "must tighten" rule (only forbid increases) | Fewer baseline commits | Slack accumulates; a file cleaned to 10 can silently creep back to 50 under a stale baseline of 50 | Rejected — that is not a ratchet |
| Enforce via `WarningsAsErrors: '*'` on changed files | Uses stock clang-tidy semantics | Still touched-files-only; still excludes GPU/ARM/MCP paths; zero-tolerance on a dirty file forces NOLINT-spam or scope creep (the ADR-0141 problem) | Rejected — it is the mechanism that failed |

## Consequences

- **Positive**:
  - "Does all code follow the repo rules?" now has a measured answer in
    `scripts/ci/tidy-baseline-*.json` instead of an assertion.
  - Rework waves (adm/vif/tools bundles, x86 macro hygiene, core plumbing,
    test tree, GPU kernels) each land as a visible baseline decrease.
  - GPU, ARM, MCP, fuzz and interop code stop being a hiding place; the
    changed-files exclusion list disappears.
  - NOLINT-without-citation becomes a counted, ratcheted number
    (ADR-0278's manual closeout gets a mechanical guard).
- **Negative**:
  - PRs that clean a file must also commit the tightened baseline; a PR
    that forgets fails with an explicit instruction, not silently.
  - A toolchain bump that changes clang-tidy's output invalidates the
    baseline; the fix is a `--write` commit in the bump PR, reviewed like
    any other baseline change (the baseline records the clang-tidy
    version and the gate warns on mismatch).
  - Roughly ten extra minutes of hosted CI per C-touching PR.
- **Neutral / follow-ups**:
  - Wire the `sycl` lane as PR-required using the oneAPI install path the
    advisory job already has; then `cuda` (CUDA toolkit headers suffice for
    `--cuda-host-only` analysis) and `hip` (ROCm headers).
  - Migrate the ratchet job onto the ADR-1140 impact planner selector
    `c_core` once PR #1196 lands (until then it uses the same early-skip
    delta probe as the changed-files job).
  - Retire the remaining carve-outs enumerated by the 2026-09-02 lint
    configuration inventory in the same PR or in the wave that owns the
    blocking toolchain; `docs/development/ci.md` carries the live list.
  - Rule 12 of `CLAUDE.md` / `AGENTS.md` §12 and
    [principles.md](../principles.md) state the whole-tree scope.

## References

- Source: maintainer direction, 2026-09-02 (`req`, verbatim): "this means
  btw: (perhaps an adr, dunno) from now on all rules are on the whole
  codebase, no restrictions anymore".
- Source: maintainer direction, 2026-08-31 (`req`, verbatim): "we still
  have upstream code that isnt reworked to our standards -> do it,
  nothing is save anymore as long as the goldens pass".
- Measurements: CPU baseline 2026-08-31 (5,354 warnings / 281 TUs, top
  files `adm_avx2.c` 570, `adm_avx512.c` 561, `cli_parse.cpp` 107); GPU
  baseline 2026-09-02 (3,543 warnings / 224 files; top files
  `cuda/integer_adm/adm_cm.cu` 174, `integer_adm_cuda.c` 136,
  `sycl/common.cpp` 102, `hip/integer_vif/vif_statistics.hip` 95). Both
  are reproduced by `scripts/ci/tidy-ratchet.py --report`.
- Related ADRs: [ADR-0141](0141-touched-file-cleanup-rule.md) (touched-file
  rule, subsumed); [ADR-0278](0278-nolint-citation-closeout.md) (NOLINT
  citations, now counted); [ADR-0313](0313-required-checks-aggregator.md)
  (aggregator contexts); [ADR-1140](1140-ci-impact-planner.md) (impact
  routing of the new job); [ADR-0100](0100-project-wide-doc-substance-rule.md)
  and [ADR-0108](0108-deep-dive-deliverables-rule.md) (per-PR
  deliverables the ratchet joins).
- Rework PRs in flight when this ADR was written: #1192 (test tree),
  #1193 (core plumbing), #1195 (x86 ADM macro hygiene), the adm/vif/tools
  bundles of wave 1b.
