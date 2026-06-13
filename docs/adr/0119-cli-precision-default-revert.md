# ADR-0119: Revert CLI precision default to %.6f to honour Netflix golden gate

- **Status**: Accepted
- **Date**: 2026-04-19
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: cli, testing, python, golden-gate
- **Supersedes**: [ADR-0006](0006-cli-precision-17g-default.md)

## Context

[ADR-0006](0006-cli-precision-17g-default.md) (2026-04-17) made the CLI's
default score-output format `%.17g` (IEEE-754 round-trip lossless), with
`--precision=legacy` as the opt-in for the pre-fork `%.6f` format. The change
landed via commit `c989fbd9 feat(cli)!: --precision flag for IEEE-754
round-trip lossless scores` on 2026-04-15 (the breaking-change marker is
correct — this *is* a behavioural break for any consumer parsing CLI output).

What the original ADR's `## Consequences` line "Python test suite comparisons
stop flaking" got wrong is that several Netflix golden tests do **exact-string
match**, not `assertAlmostEqual`. Examples:

- [`python/test/command_line_test.py:217`](../../python/test/command_line_test.py#L217)
  asserts the literal substring
  `<metric name="psnr_y" min="29.640688" max="34.760779" mean="30.755064"
  harmonic_mean="30.727905" />` is present in the XML output.
- [`python/test/vmafexec_test.py`](../../python/test/vmafexec_test.py) uses
  `assertAlmostEqual` for some checks but the file also exercises the binary's
  exact stderr/XML format in others.

Under the new `%.17g` default the actual output becomes
`min="29.640687875289579" max="34.760778811442201"
mean="30.755064021048963" harmonic_mean="30.72790477866663"` — the numbers
round-trip-equal the golden values, but the *strings* differ, so
`assertTrue('<metric ...>' in fc)` fails.

CLAUDE.md §8 (the project's hard rule for every session) states: **"Never
modify Netflix golden assertions."** They are the canonical source of truth
for VMAF numerical correctness in this fork; they ship as a required CI
status check. The ADR-0006 default therefore can't stand: it puts the binary
in a state where the only way to get past the golden gate is to either
(a) modify the assertions (forbidden), (b) thread `--precision=legacy`
through every call site (intrusive, third-party callers like FFmpeg can't
discover this), or (c) bypass the gate (also forbidden).

The breakage was latent on master from 2026-04-15 to 2026-04-19 because
ADR-0115's CI trigger consolidation hadn't yet routed `Run tox tests
(ubuntu)` through master-targeting PRs at the same SHA the precision change
landed at. PR #50's docker-job fix-up was the first master-targeting PR
after both changes were live, which surfaced the regression.

The original round-trip-lossless rationale (cross-backend exact diffing) is
still correct *as a use case*; it just doesn't justify being the default.
Backend-comparison work is opt-in by definition (you're explicitly
diffing two runs); golden-gate compliance is the universal default for
every other invocation.

## Decision

**The CLI's default score-output format is `%.6f`, matching Netflix's
pre-fork output exactly.** Round-trip-lossless `%.17g` is opt-in via
`--precision=max` (alias `full`).

Concretely:

- `core/tools/cli_parse.c:43` — `VMAF_DEFAULT_PRECISION_FMT` flips from
  `"%.17g"` to `"%.6f"`. New macro `VMAF_LOSSLESS_PRECISION_FMT "%.17g"` is
  what `--precision=max|full` resolves to.
- `core/src/output.c:29` — `DEFAULT_SCORE_FORMAT` flips from `"%.17g"` to
  `"%.6f"`. Library callers passing `score_format=NULL` to
  `vmaf_write_output_with_format()` get golden-compatible output by default.
- `python/vmaf/core/result.py:132,153` — diagnostic per-frame and aggregate
  formatters revert from `:.17g` to `:.6f`.
- `--precision=legacy` is preserved as an explicit alias for the default so
  existing scripts that pass it don't break (it is now the same as omitting
  the flag, but that's harmless).
- `--precision=N` (1..17) and `--precision=max|full` semantics are unchanged.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep `%.17g` default; modify the goldens to use `assertAlmostEqual` everywhere | Round-trip lossless stays the universal default. | Violates CLAUDE.md §8 hard rule. Loses bit-exact regression detection on the CPU path. Disturbs Netflix's audit-trail story for the fork. | Hard rule forbids modifying golden assertions. |
| Keep `%.17g` default; have the Python wrapper `ExternalProgramCaller` inject `--precision=legacy` automatically | Wrapper code can be fixed once. | CLI default still surprises every other caller (FFmpeg's `vf_libvmaf` filter, third-party scripts, MCP server, CI helpers, etc.). The wrapper-injected fix is invisible to anyone reading the CLI's `--help`. Defeats the principle of "least surprise" twice — once for callers expecting Netflix output, once for callers reading the wrapper. | Hides the real default behind a Python layer; non-Python callers still hit the surprise. |
| Drop `--precision` entirely; revert `c989fbd9` wholesale | Simplest possible state. | Loses a genuinely useful capability (round-trip-lossless cross-backend diff). The flag itself is fine; only the default was wrong. | Throwing the baby out with the bathwater. |
| Make the default conditional (e.g. `%.17g` if stdout is a pipe, `%.6f` if a TTY) | Could plausibly preserve both behaviours. | TTY-detection-driven defaults are user-hostile (CI redirects everything; behaviour silently flips between developer and CI). | Magic defaults are worse than explicit flags. |
| Revert default to `%.6f` (chosen) | Restores Netflix-compat by default, opt-in stays available, no API changes. | Round-trip-lossless callers must explicitly pass `--precision=max`. Documented loss; intentional. | Smallest change that honours the hard rule. |

## Consequences

- **Positive**: CLAUDE.md §8 golden gate is satisfied with no per-call-site
  changes. Third-party callers (FFmpeg `vf_libvmaf`, MCP server, CI scripts)
  get Netflix-compatible output by default with no awareness needed. The
  `--precision=max` opt-in keeps the round-trip-lossless capability
  available for cross-backend diffing per ADR-0009 and the
  `cross-backend-diff` skill.
- **Negative**: Anyone who started depending on `%.17g` being the default
  between 2026-04-15 and 2026-04-19 (the four-day window where ADR-0006
  was in effect) needs to either pass `--precision=max` explicitly or
  accept the format change. The fork hasn't shipped a release in that
  window, so the externally-visible blast radius is zero.
- **Neutral / follow-ups**: ADR-0006 is marked Superseded; its body is
  frozen per the ADR-maintenance rule (ADR-0028/ADR-0106). Documentation
  under [`docs/usage/precision.md`](../usage/precision.md) is rewritten to
  reflect the new default. The `cross-backend-diff` skill and benchmark
  harness should default to passing `--precision=max` themselves so they
  continue to detect ULP-level differences.

## References

- Source: PR #50 CI failure on `Ubuntu gcc static` tox step
  (`test_run_vmafexec FAILED`); per user direction, fix the precision
  default rather than modify the golden assertions or skip the test.
- CLAUDE.md §8 — "Never modify Netflix golden assertions" hard rule.
- [feedback_no_skip_shortcuts.md](../../.claude/projects/-home-kilian-dev-vmaf/memory/feedback_no_skip_shortcuts.md)
  — investigate-root-cause memory rule.
- Commit `c989fbd9` — original `--precision` flag introduction (the
  breaking-change default flip that this ADR reverts).
- ADR-0028 / ADR-0106 — ADR-maintenance immutability rule (why ADR-0006's
  body stays frozen even though its decision is reversed).
- ADR-0115 — CI trigger consolidation that surfaced the latent break.
