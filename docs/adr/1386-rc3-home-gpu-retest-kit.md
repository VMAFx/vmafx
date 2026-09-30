<!-- markdownlint-disable MD013 MD060 -->
# ADR-1386: One script runs the home GPU box's RC3 verify commands, row by row, under per-device locks

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: tooling, testing, verification, rc3, cuda, hip, sycl, fork-local

## Context

RC3 work moved from the office box (Arc B580, UHD 770) to `ryzen-4090-arc` (RTX 4090, Arc A380, gfx1036 iGPU). Fifteen open rows of [`docs/state.md`](../state.md) carry verify-and-time commands for that box, and one closed row still asks for an A380 check of the oneAPI release image. The rows were written by different changes over two days, so their commands differ in fixtures, frame counts, tolerances, and in whether they call `scripts/dev/speed_gpu_parity.py` or spell out `vmaf` runs and a Python one-liner. Run by hand, that is about seventy commands per backend. Several open pull requests (#1636 HIP parity, #1637 CUDA parity, #1639 CUDA cambi and SpEED, #1630 SYCL strict floating point) are to be measured against what `master` gives on these devices. The handoff issue #1641 names a script that runs them all as the first task at home.

The box is also shared: several agents build and run on its three GPUs at once. A timing taken while another job holds the GPU measures the other job, and two parity runs on one device can each slow the other past a timeout.

## Decision

`scripts/dev/rc3-home-gpu-retest.sh` holds one entry per `docs/state.md` row and backend. Each entry spells out its row's commands in bash (the JSON comparisons, medians and summaries live in `scripts/dev/rc3_retest_helpers.py`); the script never reads `docs/state.md` at run time. A pull request that adds or changes a row's commands for this box changes the entry in the same pull request. A test checks that every entry names a row that exists, and a pre-commit hook runs it when the kit or `docs/state.md` changes.

Every device run holds that device's `flock` (`cuda-4090.lock`, `hip-gfx1036.lock`, `sycl-a380.lock`), taken before a timing block's clock starts and held through its repetitions, and the kit never holds two device locks at once. CPU reference runs take no lock. Each entry keeps its JSON output, and `--baseline DIR` compares a later run's output with an earlier one, which is how the rows' "before" and "after" runs are done. Timings record the load average and, for CUDA, what the 4090 already had in use.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Chosen**: explicit entries per row in one bash script, JSON logic in a small Python helper | each command is readable next to its row; a row's quirks (serial CPU for motion, 48 vs 22 frames, 2.2e-15 for cambi) stay exact; `--dry-run` shows what will run | a row edit needs a matching entry edit | — |
| Parse the commands out of `docs/state.md` at run time | never out of step with the rows | the rows are prose with inline code, shell loops and placeholders ("the ported build", "N = 2 and 22"); a parser would guess, and a wrong guess reports a wrong verdict | rejected: the rows are written for people, not for a parser |
| Extend `scripts/dev/speed_gpu_parity.py` to cover every row | one tool, already used by five rows | the other rows compare different keys with different bounds, need `feature_backends` and fallback-warning checks, meson tests, and a Docker image; the script would grow a row table of its own | rejected: keep it the per-feature parity tool it is, and call it from the kit where a row does |
| Run every entry in the `vmaf-dev-mcp` container | the container is the canonical environment | the rows are written for a host `build/` and the release-image row runs Docker itself; device locks are host files | rejected for this kit; the container remains the place for published numbers (ADR-1102) |
| No device locks; ask for an idle box | simpler | the box is shared by design; an unlocked timing measures whichever job ran alongside | rejected: correctness runs must not collide, and timings need to say what they shared |
| One lock for the whole kit run | simple | holds all three GPUs for an hour, blocking every other job | rejected: per-device locks, released between runs |

## Consequences

- **Positive**: one command gives a dated, per-row record of what `master` does on this box, and the same command with `--baseline` gives the before/after comparison the rows ask for. Findings that only show on real hardware (a row whose command errors on `master`, a build option that does not build) surface in one pass.
- **Negative**: the entries duplicate the rows' commands, so they can drift; the entry test only proves that each entry's row exists, not that its commands still match the row's text.
- **Neutral / follow-ups**: rows that the open pull requests add for this box (for example `T-CUDA-FP-CONTRACT-DEFAULT-2026-09-29` and `T-HIP-FP-CONTRACT-DEFAULT-2026-09-29` from #1630) get entries when they land. Whether a check should require an entry for every row that names `ryzen-4090-arc` is left open.

## References

- Handoff issue [#1641](https://github.com/VMAFx/vmafx/issues/1641), "Home retest kit" (req, verbatim): "Every open CUDA/HIP row in `docs/state.md` carries its own verify-and-time commands (grep `ryzen-4090-arc`); a `scripts/dev/rc3-home-gpu-retest.sh` that runs them all is the first home task."
- Paraphrased from the dispatching instruction for this change: encode each command explicitly with its row id instead of parsing the markdown, let the user pick backends, a build directory, `--list` and `--only`, select the A380 and the gfx1036 explicitly, hold the per-device lock for every device run, and write a log per row plus a summary table.
- [ADR-1185](1185-backend-perf-baseline-methodology.md): median-of-N timing and recording the load, which the kit follows.
- [ADR-0165](0165-state-md-bug-tracking.md): `docs/state.md` as the bug ledger the entries point at.
- [Research-1386](../research/1386-rc3-home-gpu-retest-master-baseline.md): the first `master` run on `ryzen-4090-arc`.
