# README status badges

The README header shows ten status badges: seven workflows (Tests, Builds,
Lint, FFmpeg, Go, Rust, Security), the OpenSSF Scorecard score, the OpenSSF
Best Practices level and the Codacy grade. They are SVG files on the `badges`
branch, drawn in the same `for-the-badge` style as every other badge on the
page ([README standard](../README-STANDARD.md#badge-block)). The README links
each one with
`https://raw.githubusercontent.com/VMAFx/vmafx/badges/<slug>.svg`.

## Where a badge value comes from

The README Badges workflow (`.github/workflows/readme-badges.yml`) is
scheduled every 15 minutes, at minutes 7, 22, 37 and 52, and runs on demand.
GitHub can start a scheduled run late, or drop it, when Actions is under load,
so 15 minutes is the intended step, not a promise:

```bash
gh workflow run readme-badges.yml -R VMAFx/vmafx
```

Its first job runs `scripts/ci/readme_badges.py spec`, which asks each source
for the current value:

| Badge | Source | Message and colour |
| --- | --- | --- |
| Tests, Builds, Lint, FFmpeg, Go, Rust, Security | GitHub API: the newest push run of the workflow on the default branch, among the last 30, that has a conclusion and was not cancelled or skipped. Runs in progress, cancelled and skipped are passed over, so a run in progress never hides a red one. | `passing` green for `success`; `failing` red for `failure`, `timed_out` or `startup_failure`; `unknown` grey for any other conclusion (`action_required`, `neutral`, `stale`) and when no run qualifies. An older run's value never stands in. |
| OpenSSF Scorecard | `https://api.scorecard.dev/projects/github.com/VMAFx/vmafx` | the score with one decimal, `8.5 / 10`; green from an unrounded 8.5, the master requirement in [the principles](../principles.md) ([ADR-1247](../adr/1247-scorecard-exact-head-gates.md)), amber from 4.0, red below. The colour follows the unrounded score, so 8.46 reads `8.5 / 10` in amber. |
| OpenSSF Best Practices | `https://www.bestpractices.dev/projects.json?url=https://github.com/VMAFx/vmafx` | `in progress` amber, `passing`, `silver` or `gold` green. |
| Codacy | `https://app.codacy.com/api/v3/analysis/organizations/gh/VMAFx/repositories/vmafx` | the grade letter; A and B green, C amber, D to F red. |

Only the GitHub API call carries the workflow token. The three public
sources get a plain request with no token. Every request goes over https
only, and a redirect is not followed: it fails as an error, so the token
never travels to another address.

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
with the time each one was read. A time without a UTC offset, or one that
cannot be read, counts as stale.

The six hours run from the read time `state.json` holds. A run whose SVGs are
all unchanged still commits once a badge's new read time is an hour or more
newer than the one on the branch (see below), so that time trails the last
successful read by less than an hour, also right after a source recovers with
its old value. A badge can therefore turn `stale` up to an hour before six
hours have passed since its last successful read, and one schedule step after,
plus any delay GitHub adds to a scheduled run.
Committing `state.json` on every run would close that gap at the cost of
about four commits an hour instead of one.

When `state.json` itself cannot be read (any answer but 404, which means the
branch is new), the run stops before it renders anything. The badges keep
their values instead of all turning `stale` at once.

## How the badges branch is written

The second job runs `scripts/ci/readme_badges.py publish`, which writes the
ten SVGs and `state.json` as the whole tree of one new commit through the
GitHub API:

- GitHub signs the commit. A commit that comes back unverified is not
  published, and the branch stays where it was.
- The branch only moves forward (`force: false`).
- When every SVG is unchanged, the last commit is less than an hour old, and
  `state.json` on the branch holds each badge's read time less than an hour
  before this run's, nothing is committed, so the branch grows by about one
  commit an hour. A badge whose source has never answered has no read time in
  either file; that counts as recorded, since there is nothing to record. A
  missing state file or badge entry, a different set of badges, or a read time
  that is unreadable, in the future or on one side only means a commit.
- A commit made with the workflow token starts no other workflow.

The job's token has `contents: write`, the narrowest right that can make a
commit. GitHub cannot limit that right to one branch; the script writes only
`refs/heads/badges`, and the master ruleset requires a pull request, so the
token cannot move `master`. No ruleset guards `badges` yet, so any other
token with `contents: write` (another workflow, an installed app, a person
with write access) can push to it too. Issue 3394 tracks a dedicated GitHub
App as the only publisher, with a `badges` ruleset that refuses every other
writer.

Both jobs run only on the default branch (`github.ref` is
`refs/heads/master`): a manual run started on another branch does nothing, so
the publish token never runs a branch's own copy of the workflow. Both jobs
check out `github.sha`, the commit the run started from, so a master push
between the two jobs cannot hand the publisher other code than the renderer
ran.

## Why a schedule and not `workflow_run`

A `workflow_run` trigger would refresh a badge within a minute of each master
run. zizmor rates `workflow_run` a dangerous trigger, and its
`branches: [master]` filter matches the branch name only, so a pull request
from a fork's branch named `master` would start it too. The schedule costs up
to 15 minutes of lag, more when GitHub delays a run, and has neither problem.

## Limits

- An SVG cannot age by itself. When the workflow itself stops (disabled, or
  scheduled runs paused after 60 days without repository activity), no run
  starts, so none turns red, and the badges keep showing the last values,
  green ones included. The signal is a missing run: the newest README Badges
  run is more than an hour old (a single scheduled run can start late or be
  dropped). Check it, then re-enable the workflow:

  ```bash
  gh run list -R VMAFx/vmafx --workflow readme-badges.yml --limit 1 --json createdAt,conclusion
  gh workflow enable readme-badges.yml -R VMAFx/vmafx
  ```

- The Praetor governance badge in the `praetor:readme-governance` block is
  written by Praetor and keeps GitHub's design.

## Checks

```bash
python3 scripts/ci/readme_badges.py check-readme --readme README.md
python3 -m pytest scripts/ci/tests/test_readme_badges.py
```

`check-readme` fails when any image in the README breaks the one design. An
image passes only when it is one of the rendered SVGs on this repository's
`badges` branch, a shields.io badge with both `style=for-the-badge` and
`labelColor=111a35`, or a file that exists inside the repository (the
banner). The repository name comes from
`.github/repository-security-policy.json`; a badge rendered for any other
repository or a fork fails. A local path fails when it names no file or
resolves outside the README's directory, through a symbolic link included.
Images inside code fences and code spans are ignored. The one exception is
the Praetor governance badge inside the `praetor:readme-governance` block;
it goes once Praetor can draw that badge in the same style (Praetor issue
1893). Each fault names the line:

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
