# ADR-0111: Coverage gate `lcov` → `gcovr` with ORT in the coverage job

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Supersedes**: [ADR-0110](0110-coverage-gate-fprofile-update-atomic.md)
- **Tags**: ci, build, dnn, testing

## Context

ADR-0110 closed the geninfo abort and fixed the inter-process `.gcda`
merge race via `-fprofile-update=atomic` + `meson test --num-processes 1`.
The follow-up CI run still surfaced two structural problems that
ADR-0110 did not (and could not) address:

1. **`dnn_api.c` reported 1176% line coverage.** The race fixes were
   correctly applied — empirically verified by the geninfo log — yet
   `lcov --capture --directory build-coverage` still summed every
   `.gcda` it found. Each `core/src/dnn/dnn_api.c` source is
   compiled into both `libvmaf.so` *and* most test binaries
   (`build-coverage/test/test_X.p/.._src_dnn_api.c.gcda`). `lcov`
   aggregates these as if they were the same compilation unit and
   sums the hit counts; when the per-CU instrumented line maps differ
   (different optimisation, different `#if` evaluation in the test
   harness vs. the .so), the resulting `hits / lines` ratio exceeds
   100%. Filtering the test paths via `lcov --remove '*/test/*'`
   removes them from the *report* but not from the aggregation.
2. **The DNN critical files are unmeasurable in the CPU coverage
   build.** `meson setup build-coverage` did *not* set
   `-Denable_dnn=enabled`, so `core/src/dnn/*.c` only compiled
   their stub branches. The 85% per-critical-file gate then enforced a
   threshold against synthetic stub coverage, not real DNN code paths.
   The companion "Tiny AI" job installs ORT and runs the dnn suite,
   but its coverage data was being thrown away.

## Decision

Two changes, applied to both the CPU and the (advisory) GPU coverage jobs:

1. **Switch from `lcov` to `gcovr`.** `gcovr` deduplicates `.gcno`
   files belonging to the same source compiled into multiple targets
   — the per-source line coverage is computed from the union of the
   compilation units rather than their sum, so the impossible 1176%
   class of failure becomes structurally impossible. `gcovr` also
   produces native Cobertura XML (for downstream tooling) and a
   `--json-summary` that the gate script can parse with `python3 -c`
   instead of grep-and-awk over `lcov --list` text. Output now lands
   as `coverage.{xml,json,txt}` in `core/build-coverage/` and the
   uploaded artifact is renamed `coverage-cpu` / `coverage-gpu`
   (was: `coverage-lcov-cpu` / `coverage-lcov-gpu`).

2. **Install ONNX Runtime (CPU) in the coverage job and build with
   `-Denable_dnn=enabled`.** Mirrors the dnn job's ORT-install step
   (the pinned `1.22.0` Linux x86_64 tarball + a hand-rolled
   `libonnxruntime.pc`). The DNN test suite then runs against real
   ORT and `core/src/dnn/*.c` contributes honest coverage. The
   85% per-critical-file gate now measures real code, not stubs.

The `-fprofile-update=atomic` build flag and the
`meson test --num-processes 1` serialisation from ADR-0110 are
retained — both races still exist and gcovr's deduplication does
not address them.

`scripts/ci/coverage-check.sh` is rewritten to consume gcovr's
`--json-summary` output. The CLI signature is unchanged
(`coverage-check.sh <summary> <overall%> <critical%>`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| `gcovr` + ORT install (chosen) | Honest per-file numbers across all critical files; gate measures real DNN code; fork-wide adoption of a tool that's better at multi-target builds | Adds gcovr install to coverage job (~10s); doubles ORT install time across two jobs (~20s); pip-installed dependency rather than apt | Cleanest end-to-end fix; both 1176% and the DNN-stub-coverage problem are root-cause-resolved |
| Scope `lcov --capture` to `--directory build-coverage/src` | Drops the duplicate test-target compilations directly; one-line diff | Loses coverage from any code only exercised when statically linked into a test (rare but real for some helpers); does NOT address the DNN-stub-coverage problem; still tied to lcov | Half-fix; the DNN gap blocks the gate even if the over-counting is resolved |
| Post-process lcov: clamp `>100%` to `100%` | Tiny diff; unblocks gate fast | Hides the underlying double-counting bug; a future file genuinely at 50% (real) + 60% (duplicate CU) becomes "100%" silently | Defeats the purpose of having a coverage gate; rejected per the no-skip-shortcuts rule |
| Promote the existing dnn job to "coverage source" + merge two coverage artifacts | Avoids duplicate ORT install; existing dnn job already runs the dnn suite | Two-job artifact merge is fragile; `coverage-cpu` vs `coverage-dnn` artifact reconciliation needs custom scripting | Bigger blast radius for the same outcome; one job that does everything is simpler |

## Consequences

- **Positive**: Coverage gate produces honest per-file numbers across
  *all* critical files, including the DNN tree. The 85% per-file bar
  is now measurable.
- **Positive**: `gcovr`'s Cobertura XML output is consumable by
  Codecov / SonarQube / GitHub's built-in code-scanning surfaces if
  we choose to wire them in later.
- **Positive**: Eliminates a class of silent coverage-gate failures
  caused by lcov's summation behaviour. Any future `.c` file built
  into multiple targets won't trip the gate spuriously.
- **Negative**: Coverage job wall-time grows by ~30–40s (gcovr
  install + ORT install + dnn build delta). Still well under the
  30-minute job budget.
- **Negative**: New per-critical-file numbers will likely show real
  gaps (existing DNN tests are limited; e.g.
  [`test_op_allowlist.c`](../../core/test/dnn/test_op_allowlist.c)
  is only 37 lines). Closing the gap requires writing tests, which is
  in scope for the same PR per user direction ("Keep 85%; write tests
  now"). Files that fall short get tests added in the same PR or a
  fast-follow PR linked to the principal-debt tracker.
- **Negative**: The supersession of ADR-0110 means CI history before
  this PR's land-date will read confusingly without the breadcrumb.
  The breadcrumb is the `Supersedes` header line above and the cross
  link in [docs/adr/README.md](README.md).

## References

- ADR-0110 race fixes (still in force): see
  [ADR-0110 §Decision items 1 and 2](0110-coverage-gate-fprofile-update-atomic.md#decision).
- Empirical evidence for the 1176% over-count after the ADR-0110
  race fixes:
  <https://github.com/VMAFx/vmafx/actions/runs/24606544171>
  (Coverage gate step at 14:20:34 — `dnn_api.c — 1176%` with the
  rest of the DNN tree at 5–14%).
- gcovr docs:
  <https://gcovr.com/en/stable/index.html>;
  the `--json-summary` schema:
  <https://gcovr.com/en/stable/output/json.html#json-summary>.
- ORT install pattern: mirrored from
  [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) job
  `dnn` "Install ONNX Runtime (CPU)" step.
- Related ADRs: [ADR-0042](0042-tinyai-docs-required-per-pr.md)
  (DNN docs requirement),
  [ADR-0102](0102-dnn-ep-fallback-order.md) (DNN EP fallback that the
  loaded ORT exercises),
  [ADR-0107](0107-tinyai-wave1-scope-expansion.md) (DNN scope).
- Source: `req` — direct user direction on this PR ("Switch lcov →
  gcovr" and "Keep 85%; write tests now" in response to two
  consecutive AskUserQuestion popups on 2026-04-18 mid-session).
