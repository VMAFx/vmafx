- **`python/test/`**: complete port of upstream's MyTestCase migration cluster
  (15 cherry-picked commits) — adopt the new test base class, port aim/adm3/
  motion3 test methods, widen macOS FP precision tolerances per upstream,
  and import upstream's own score-value updates as the new golden truth
  per CLAUDE §1 ("the rule prevents fork-local drift, NOT importing
  Netflix's own value updates"). Supersedes #459 (partial port). See
  ADR-0326's `### Status update 2026-05-08: complete migration landed`
  appendix.
