<!-- markdownlint-disable MD060 -->
# Research-0922: Coverage ratchet sizing + per-PR delta gate design

- **Date**: 2026-05-31
- **Author**: lusoris (Claude-assisted)
- **ADR**: [ADR-0922](../adr/0922-coverage-ratchet-aggressive.md)
- **PR**: chore/coverage-ratchet-aggressive

## Question

How aggressively can the Coverage Gate floors be raised today without
catching the existing master tree below the bar, and what shape of
per-PR ratchet best prevents one-PR-at-a-time decay between absolute-floor
trips?

## Inputs

- `scripts/ci/coverage-check.sh` floor-history block (ADR-0637 narrative):
  40 % original → 37 % post-merge-burst.
- `PER_FILE_MIN` map current values: ort_backend.c=78, dnn_api.c=78,
  tiny_extractor_template.h=10.
- `docs/principles.md §3` aspirational targets: 70 % overall, 85 %
  security-critical.
- ADR-0114 measurement table (PR #46):
  `dnn_api.c` 79.5 %, `ort_backend.c` 79.3 %.
- Operational observation: small-loop gcov hit-count variance moves
  per-file percentages by roughly 0.1–0.2pp between identical runs on
  ubuntu-latest (consistent across multiple Coverage Gate trips on
  master).

## Method

1. **Headline-floor sizing.** Pick a value that (a) is meaningfully
   higher than the measured-floor 37 % so it exerts upward pressure,
   (b) is below the most recent measured CI value on master so the
   ratchet doesn't trip immediately, and (c) is a round number for
   review readability. The 2026-05-29 Coverage Gate run reported
   overall coverage at ~63 % per the CI logs; 60 % gives ~3pp of
   headroom for normal variance + small-PR drift.
2. **Critical-floor sizing.** The aspirational 85 % is unchanged from
   ADR-0114; ratcheting to 90 % matches what the non-exempt critical
   files (`opt.c` at 100 %, `op_allowlist.c` at 100 %, `tensor_io.c` at
   97.2 %, `read_json_model.c` at 88.2 %, `model_loader.c` at 86.4 %,
   `onnx_scan.c` at 94.6 %) currently report. `model_loader.c` at
   86.4 % is the closest non-exempt file to the new bar; lifting it +4pp
   is the same effort as the +5pp per-file ratchet on the exempt
   entries (one or two targeted unit tests per file).
3. **Per-file ratchet sizing.** +5pp uniform on every `PER_FILE_MIN`
   entry: large enough to be visible, small enough to be reachable
   today. ADR-0114 itself documented +5pp as "~1.3pp slack" above the
   measured value (79.3 % → 78 %); the inverse direction (current
   79.3 % → 83 %) needs roughly the same effort but acts as forward
   pressure rather than safety net. `tiny_extractor_template.h` jumps
   from 10 % → 15 %, which exercises one more menu helper than the
   current extractors call — a one-test add that follows the same
   pattern as the recent ORT accessor coverage (rebase-notes
   2026-05-30 entry).
4. **Delta-tolerance sizing.** Set tolerance comfortably above
   measured variance (0.2pp) but below typical "real" regressions
   (1pp+ when a meaningful test gets disabled or a new code path
   ships without coverage). 0.5pp falls in the middle — false
   positives from variance are vanishingly rare, real regressions
   trigger reliably.
5. **Scope of delta-gate per-file scoring.** Limit to files touched
   by the PR's diff: a PR can't be blamed for the existing test
   suite drifting on files it doesn't even import. The overall
   delta still catches whole-tree drift caused by the PR (e.g. a
   shared-library refactor that changes which TUs link into which
   test binaries).
6. **Grace window length.** 30 days matches the typical fork
   merge-train horizon (sweep of in-flight PRs clears in ~3–4
   weeks per observation). Long enough for honest in-flight work
   to converge, short enough that the rule self-disables before
   the next quarterly planning round.

## Findings

| Dimension | Old | New | Justification |
|---|---|---|---|
| `OVERALL_MIN` | 37 | 60 | +23pp ratchet; below current measured (~63 %) so no immediate trip; midpoint toward `principles.md §3` 70 % goal. |
| `CRITICAL_MIN` | 85 | 90 | +5pp ratchet; non-exempt files all currently report ≥86.4 %. |
| `ort_backend.c` per-file | 78 | 83 | +5pp; current measured 79.3 % → needs ~3pp investment, which is one fault-injection test. |
| `dnn_api.c` per-file | 78 | 83 | +5pp; current measured 79.5 %; `has_norm` dead-branch deletion (referenced in ADR-0114) gets it +3pp for free. |
| `tiny_extractor_template.h` per-file | 10 | 15 | +5pp; one more menu helper exercised by an existing extractor TU. |
| Delta-gate tolerance (overall) | n/a | 0.5pp | Above measured ~0.2pp variance; below ~1pp typical real-regression. |
| Delta-gate tolerance (per-touched-file) | n/a | 0.5pp | Same justification as overall. |

The per-file ratchets are reachable today; the headline ratchet provides
~3pp of headroom against the most recent CI measurement; the delta gate
defaults are calibrated against measured gcov variance.

## Risks and mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Delta gate fires on variance alone | Low | 0.5pp tolerance is 2–3× measured variance; ADR-0922 self-supersedes if false positives appear repeatedly. |
| Base-coverage build adds CI latency | Certain (~4 minutes) | Lean CPU-only build, no ORT install, no Python suite — only enough to populate per-file gcov data. PR-only (no impact on master pushes). |
| Grace-window reviewer misapplication | Possible | The window is short (30 days), the exemption is loud (PR title timestamp vs. 2026-05-31 is trivial to check), and it self-disables on 2026-06-30. |
| New file shipped at low coverage bypasses delta gate | Possible | Absolute floors still apply (critical files in particular); a follow-up ADR could add a per-new-file floor if drift shows up in practice. |
| `git merge-base` failure (shallow clone) | Possible if workflow restructured | Documented in `scripts/ci/AGENTS.md` and root `AGENTS.md` §13 invariant; `fetch-depth: 0` is mandatory. |

## Recommendation

Land the ratchet as specified above with the 30-day grace window. Plan
a follow-up ADR to ratchet 60 → 70 once two consecutive CI weeks show
overall sitting above 65 %. Track delta-gate false-positive rate over
the first month; tighten to 0.25pp if false positives stay below 1/week.

## References

- ADR-0114: per-file override map (the table this ADR ratchets).
- ADR-0110, ADR-0117, ADR-0637: prior Coverage Gate ADRs (gcovr
  migration, warning suppression, floor history).
- `docs/principles.md §3`: aspirational targets.
- `req` (paraphrased): user directed an aggressive ratchet plus a
  per-PR delta gate, with a grace period and an exception process
  gated on a follow-up ADR.
