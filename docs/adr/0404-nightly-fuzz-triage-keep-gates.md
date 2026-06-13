<!-- markdownlint-disable MD060 -->
# ADR-0404: Keep `nightly.yml` + `fuzz.yml` red until underlying bugs land

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: lusoris, Claude
- **Tags**: ci, testing, fuzzing, security

## Context

Research-0089 (PR #525) §5 flagged that `nightly.yml` and `fuzz.yml`
have **0 successful runs in the last 50** runs each. Empirical triage
against `gh run list --workflow … --limit 50 --json conclusion,event`
on 2026-05-09 confirmed:

- `nightly.yml` — 23 consecutive `schedule` failures on `master`
  (oldest visible run 2026-04-16, latest 2026-05-08). The
  `clang-tidy-full` and `netflix-benchmark` jobs succeed; only the
  `ThreadSanitizer` job fails. TSan reports a **real data race in
  `div_lookup_generator` at `core/src/feature/integer_adm.h:32-38`** —
  a static `div_lookup[65537]` table populated concurrently by
  multiple worker threads spawned from `vmaf_thread_pool_create` in
  `core/src/thread_pool.c:169`. Failing tests: `test_model`,
  `test_framesync`, `test_pic_preallocation` (3 of 50).

- `fuzz.yml` — 4 consecutive `schedule` failures on `master`
  (2026-05-05 through 2026-05-08). The `fuzz_yuv_input` and
  `fuzz_cli_parse` matrix legs succeed; only `fuzz_y4m_input` fails.
  Crash: `AddressSanitizer SEGV` in `fread` at
  `core/tools/y4m_input.c:877` from a `YUV4MPEG2 W-8 H4 F30:1 Ip
  A1:1 C422` reproducer (negative width parses but leaves
  `_y4m->dst_buf == NULL` while `dst_buf_read_sz > 0`, NULL-deref on
  the `fread` destination). Distinct from the Y4M-411-OOB crash that
  PR #357 / commit `05ba29a6` already fixed in `y4m_input.c:507`.

Both gates are doing exactly what they were scaffolded to do
(ADR-0015 for nightly TSan, ADR-0270 + ADR-0311 for libFuzzer):
surfacing real bugs. The triage decision is what to do with the
red signal until the underlying fixes land.

## Decision

We will **keep both workflows running, unmodified**. No
`continue-on-error`, no matrix-leg deletion, no
`workflow_dispatch`-only gating. The bugs each gate has surfaced are
documented in `docs/state.md` with explicit reopen triggers; once the
fix PRs land, the next nightly run will return green automatically.

This is the application of memory `feedback_no_test_weakening` to a
CI gate that has been red for 23+ days: the gate is correctly
catching real defects, and the cost of a CI red badge is lower than
the cost of silencing a working detector. Both findings (the ADM
init race and the negative-width Y4M SEGV) are out-of-scope for
this triage PR and will be fixed in dedicated follow-up PRs.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Keep gates running, document the open bugs** (chosen) | Honours `feedback_no_test_weakening`; the gate keeps watching for regressions on the *other* harnesses while the known bug is in flight; reopen is automatic | CI red badge persists until fixes land | Aligned with the user's standing rule that a working detector is never silenced to make CI green |
| Add `continue-on-error: true` to the failing jobs / matrix legs | CI badge turns green | Silences a working detector; later regressions in the same code path get masked under a "yellow" marker that humans habituate to ignoring | Direct violation of `feedback_no_test_weakening`; the rule explicitly covers "skipping/disabling failing tests" |
| Disable both workflows entirely | Stops the noise | Loses ~24 h of TSan / fuzz coverage on the *passing* paths every day; the ADM race is a known data race the user explicitly wants surfaced | Throws out the working signal with the failing signal |
| Fix both bugs in this PR | Returns CI to green | Two unrelated fixes in a triage PR; the ADM race needs a `pthread_once` / `call_once` rework that touches a load-bearing init path (NASA/JPL Power-of-10 review territory); the y4m fix needs a header-validation hardening sweep that should be co-designed with `fuzz_y4m_input.c` filter rules | Scope creep; a triage PR ships triage, not unrelated bug fixes |

## Consequences

- **Positive**: Both gates continue to surface regressions on the
  passing harnesses (TSan covers `test_pic_preallocation` /
  `test_framesync` / `test_model` *plus* 47 other tests; fuzz covers
  `fuzz_yuv_input` + `fuzz_cli_parse` + the corpus-only path of
  `fuzz_y4m_input`). The CI red badge becomes load-bearing —
  reviewers see exactly two known-open bugs and any *new* failure is
  immediately distinguishable.
- **Negative**: Until the two fixes land, the badge stays red and
  any new fuzz / TSan finding is queued behind the existing two
  rather than being immediately distinguishable. Mitigated by the
  state.md rows pinning the *current* failing tests + harness so a
  third row (a new finding) is obviously additive.
- **Neutral / follow-ups**:
  - Fix PR for the ADM init race — recommend wrapping
    `div_lookup_generator()` in `pthread_once` or moving it to a
    one-shot init at `vmaf_init` boundary before any worker thread
    spawns.
  - Fix PR for the negative-width Y4M SEGV — header parser needs to
    reject `W <= 0` / `H <= 0` before any allocation; the fuzz
    reproducer (artifact `crash-645a8f241b71d80ff496c10984d9b493d03dbfe1`,
    auto-uploaded by run 25538384046) should be promoted to
    `core/test/fuzz/y4m_input_known_crashes/` until the fix lands.
  - Both rows added to `docs/state.md` Open section in the same PR
    as this ADR; reopen triggers cite the fix-PR commit SHAs (filled
    in when the fixes land).

## References

- Research-0089 (PR #525) §5 — flagged the 0/50 success rate.
- ADR-0015 — nightly TSan gate scaffold.
- ADR-0270 — libFuzzer scaffold.
- ADR-0311 — fuzz harness expansion.
- Memory `feedback_no_test_weakening` — never weaken a test to make
  it pass.
- `gh run list --workflow nightly.yml --limit 50` (2026-05-09):
  23 visible `schedule` runs, all `failure`.
- `gh run list --workflow fuzz.yml --limit 50` (2026-05-09):
  4 visible `schedule` runs, all `failure`.
- Source: `req` — user task body 2026-05-09 ("Investigation-then-fix
  task: nightly.yml and fuzz.yml workflows have 0 successful runs …
  document the deferral with explicit reopen trigger + state.md
  row").
