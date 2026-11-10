---
paths:
  - scripts/ci/readme_badges.py
  - scripts/ci/tests/test_readme_badges.py
  - tools/badges/*
  - .github/workflows/readme-badges.yml
  - README.md
invariant: README badge order = `BADGES`; README images pass `check-readme`; publish moves `refs/heads/badges` only, unforced.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# README status badges (ADR-3519)

`scripts/ci/readme_badges.py` reads badge values; `tools/badges/render.mjs` draws SVGs; `.github/workflows/readme-badges.yml` publishes SVGs to `badges` branch. Human guide: `docs/development/readme-badges.md`.

- Order + labels: README header badges, alt texts, order = `BADGES` tuple; workflow badge label = that workflow's `name:`. `test_readme_badges.py` reads `README.md`; new badge changes both.
- Run choice: workflow badge = newest push run on default branch with conclusion, not cancelled or skipped (`PASSED_OVER`). `success` -> passing; `failure`, `timed_out`, `startup_failure` -> failing; other conclusion -> `unknown`, never older run's value. Never "latest run".
- Scorecard: green from unrounded `SCORE_GREEN` = 8.5 (master requirement, `docs/principles.md`, ADR-1247); shown value rounded to 1 decimal, colour from unrounded score.
- Renderer allow-list: keys `slug`, `label`, `message`, `color` only; text `[\x20-\x7e]{1,40}`; colour `#[0-9a-f]{6}`. `TEXT` / `COLOR` in Python and `render.mjs` stay equal (test compares both).
- Carry: failed source keeps last value `STALE_AFTER` = 6 h from `state.json` read time, then `stale` grey; naive or unreadable time = stale (`read_time`). Recorded time trails last successful read < `QUIET_WINDOW` (`reads_recorded`), recovery with same value included (regression test pins case); `stale` <= 1 h early, <= 1 step late; accepted trade-off vs 4 commits/h. GitHub delays or drops scheduled runs at times: 15 min = intended step, never stated as max badge age. `verdict` step exits 1 on any failed source.
- State read: `previous_state` 404 = new branch, empty; any other failure -> `BadgeError`, run stops, nothing rendered or published. Never return empty state on failed read.
- Publish: one commit via Git data API; GitHub-signed commit required (unverified -> no ref move); `force: false`; ref path only `refs/heads/badges`; no commit only when SVGs unchanged, head < `QUIET_WINDOW` = 1 h old, and branch `state.json` holds each badge read time 0 to < `QUIET_WINDOW` before this run's (`reads_recorded`). No read time on both sides (source never answered) = nothing to record, counts as recorded (`read_lag` 0; `test_no_read_time_on_both_sides_commits_nothing`). Missing state file or entry, other badge set, unreadable, future or one-sided time -> commit.
- Tokens: `GITHUB_TOKEN` only to `api.github.com`; Scorecard, Best Practices, Codacy reads carry no token.
- Requests: every call through `ci_tier.https_json` (https only, size cap: public `MAX_RESPONSE_BYTES`, GitHub API `ci_tier.API_MAX_BYTES`); no second request helper. `HTTPS_OPENER` = no redirect handler: 3xx = `HTTPError`, `Authorization` never resent elsewhere. JSON answers read with `field()`, never bare indexing.
- Design check: `readme_badges.py check-readme` (hook `readme-badge-design`) fails README image unless rendered `badges`-branch SVG, shields.io badge with `style=for-the-badge` + `labelColor=111a35`, or repo-local file that exists. Rendered + governance badges -> home repository required (`home_repository()` from `.github/repository-security-policy.json`, case-insensitive); other repo or fork fails. Local path: `missing_file` resolves symlinks, fails outside README dir or no file. Fences + code spans ignored. Only exception: `praetor-docs.yml/badge.svg` inside `praetor:readme-governance` block (`governance_badge()`) until Praetor issue 1893 lands; then drop exception.
- Rights: render job `actions: read` + `contents: read`; publish job `contents: write` only; workflow top level `permissions: {}`. No `workflow_run` trigger (zizmor dangerous trigger; fork branch named `master` matches). Workflow guard + checkout rules: `.github/AGENTS.d/readme-badges.md`.
