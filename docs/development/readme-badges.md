# README status badges

The README header shows ten status badges: seven workflows (Tests, Builds,
Lint, FFmpeg, Go, Rust, Security), the OpenSSF Scorecard score, the OpenSSF
Best Practices level and the Codacy grade. They are SVG files on the `badges`
branch, drawn in the same `for-the-badge` style as every other badge on the
page ([README standard](../README-STANDARD.md#badge-block)). The README links
each one with
`https://raw.githubusercontent.com/VMAFx/vmafx/badges/<slug>.svg`.

## Where a badge value comes from

The README Badges workflow (`.github/workflows/readme-badges.yml`) runs every
15 minutes, at minutes 7, 22, 37 and 52, and on demand:

```bash
gh workflow run readme-badges.yml -R VMAFx/vmafx
```

Its first job runs `scripts/ci/readme_badges.py spec`, which asks each source
for the current value:

| Badge | Source | Message and colour |
| --- | --- | --- |
| Tests, Builds, Lint, FFmpeg, Go, Rust, Security | GitHub API: the newest completed push run of the workflow on the default branch whose conclusion is `success`, `failure`, `timed_out` or `startup_failure`. Cancelled, skipped and waiting runs are passed over, so a run in progress never hides a red one. | `passing` green, `failing` red, `unknown` grey when none of the last 30 runs decided. |
| OpenSSF Scorecard | `https://api.scorecard.dev/projects/github.com/VMAFx/vmafx` | the score with one decimal, `7.0 / 10`; green from 7.0, amber from 4.0, red below. The colour follows the shown value, so 6.96 reads `7.0 / 10` in green. |
| OpenSSF Best Practices | `https://www.bestpractices.dev/projects.json?url=https://github.com/VMAFx/vmafx` | `in progress` amber, `passing`, `silver` or `gold` green. |
| Codacy | `https://app.codacy.com/api/v3/analysis/organizations/gh/VMAFx/repositories/vmafx` | the grade letter; A and B green, C amber, D to F red. |

Only the GitHub API call carries the workflow token. The three public
sources get a plain request with no token.

The workflow writes the list to `build/badges-spec.json`, and
`tools/badges/render.mjs` draws one SVG per entry with `badge-maker`, the
renderer shields.io uses, at the version pinned in
`tools/badges/package-lock.json`. The renderer accepts only the four keys
`slug`, `label`, `message` and `color`, printable ASCII text up to 40
characters and a `#rrggbb` colour, so a source cannot put markup into the SVG.

## When a source fails

A source that times out or answers something unexpected keeps its last value
for six hours. After that its badge reads `stale` in grey. Either way the run
turns red at its last step (`readme_badges.py verdict`), so the failure shows
in the Actions tab the same day, not when someone notices an old badge.

The last values live in `state.json` on the `badges` branch, next to the SVGs,
with the time each one was read.

## How the badges branch is written

The second job runs `scripts/ci/readme_badges.py publish`, which writes the
ten SVGs and `state.json` as the whole tree of one new commit through the
GitHub API:

- GitHub signs the commit. A commit that comes back unverified is not
  published, and the branch stays where it was.
- The branch only moves forward (`force: false`).
- When every SVG is unchanged and the last commit is less than an hour old,
  nothing is committed, so the branch grows by about one commit an hour.
- A commit made with the workflow token starts no other workflow.

The job's token has `contents: write`, the narrowest right that can make a
commit. GitHub cannot limit that right to one branch; the script writes only
`refs/heads/badges`, and the master ruleset requires a pull request, so the
token cannot move `master`.

## Why a schedule and not `workflow_run`

A `workflow_run` trigger would refresh a badge within a minute of each master
run. zizmor rates `workflow_run` a dangerous trigger, and its
`branches: [master]` filter matches the branch name only, so a pull request
from a fork's branch named `master` would start it too. The schedule costs up
to 15 minutes of lag and has neither problem.

## Limits

- An SVG cannot age by itself. When the workflow itself stops (disabled, or
  scheduled runs paused after 60 days without repository activity), the
  badges keep showing the last values. The red run and the README Badges
  entry in the Actions tab are the signal.
- The Praetor governance badge in the `praetor:readme-governance` block is
  written by Praetor and keeps GitHub's design.

## Checks

```bash
python3 scripts/ci/readme_badges.py check-readme --readme README.md
python3 -m pytest scripts/ci/tests/test_readme_badges.py
```

`check-readme` fails when any image in the README breaks the one design. An
image passes only when it is one of the rendered SVGs on the `badges` branch,
a shields.io badge with both `style=for-the-badge` and `labelColor=111a35`, or
a file inside the repository (the banner). Images inside code fences and code
spans are ignored. The one exception is the Praetor governance badge inside
the `praetor:readme-governance` block; it goes once Praetor can draw that
badge in the same style (Praetor issue 1893). Each fault names the line:

```text
README.md:117: https://img.shields.io/badge/FFmpeg-n9.0.2-8b5cf6?style=flat-square&labelColor=111a35: a shields.io badge needs style=for-the-badge
```

The `readme-badge-design` pre-commit hook runs the check whenever `README.md`
or the script changes, at commit and at push, and the `test-readme-badges`
hook runs both test files.

The tests cover every value rule above, the six-hour carry, the publish rules,
the design check and the README itself: the badge order and alt texts in
`README.md` must match `BADGES` in `scripts/ci/readme_badges.py`, each
workflow badge's label must be that workflow's `name:`, and the text and
colour patterns of `render.mjs` must equal the Python ones.
