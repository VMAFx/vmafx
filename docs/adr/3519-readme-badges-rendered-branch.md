<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3519: Render the README status badges in one style onto a `badges` branch

- **Status**: Accepted
- **Date**: 2026-11-10
- **Deciders**: Lusoris
- **Tags**: `ci`, `docs`, `readme`, `supply-chain`

## Context

[ADR-3366](3366-readme-presentation-standard.md) puts one `for-the-badge`
badge block under the README banner. The header still mixed four designs:
the seven workflow badges were GitHub's own `badge.svg` images, which come in
one fixed design; the Best Practices and Codacy badges came from their
services in their own designs; the OpenSSF Scorecard badge was a static
shields.io image that showed no score at all; and the backends row used
`flat-square`. GitHub's `badge.svg` cannot be restyled, and the shields.io
workflow badge reads that same image
(`services/github/github-actions-workflow-status.service.js` in the shields
repository, read 2026-11-10), so neither gives this project a say in which run
a badge reports.

## Decision

We render the ten status badges ourselves.
`.github/workflows/readme-badges.yml` is scheduled every 15 minutes and runs
on demand: its render job (`actions: read`, `contents: read`) runs
`scripts/ci/readme_badges.py spec`, which reads the newest decisive push run of
each badged workflow on the default branch and the public Scorecard, Best
Practices and Codacy APIs, and `tools/badges/render.mjs` draws the SVGs with
`badge-maker` 6.0.0, the shields.io renderer, pinned in
`tools/badges/package-lock.json`. Its publish job (`contents: write`) commits
the SVGs and `state.json` to the `badges` branch through the GitHub API, and
the README links `https://raw.githubusercontent.com/VMAFx/vmafx/badges/<slug>.svg`.
Both jobs run only on the default branch and check out the commit the run
started from. A source that fails turns the run red at once; its badge keeps
the last value for six hours, then reads `stale`. Every other README badge,
the backends row included, uses `for-the-badge`; the one exception is the
Praetor governance badge, which Praetor writes. The full rules are in
[docs/development/readme-badges.md](../development/readme-badges.md).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Rendered badges on a `badges` branch, schedule trigger (chosen) | One design; the project decides which run a badge shows; Scorecard shows its score; no third-party request per page view | About 15 minutes of lag, more when GitHub delays a scheduled run; a job with `contents: write`; a branch that grows by about one commit an hour | Chosen |
| Same, triggered by `workflow_run` | Badge within a minute of a master run | zizmor rates `workflow_run` a dangerous trigger; its `branches: [master]` filter matches a fork's branch named `master` too | The 15-minute lag is the smaller cost |
| shields.io dynamic URLs with `style=for-the-badge` | No workflow or branch; one design | Each page view depends on shields.io; the workflow badge scrapes GitHub's `badge.svg`, so run selection stays GitHub's; Scorecard and Codacy would need per-service shields routes | Keeps a third party on every page view and leaves run selection out of reach |
| Keep each service's own badge | Nothing to build | Four designs in one block, against ADR-3366; the Scorecard badge shows no score | It is the defect this decision fixes |
| Badges as GitHub Pages files | Same rendering | Pages deploys the documentation site; a second deploy path for ten files | The branch needs no Pages change |

## Consequences

- **Positive**: one badge design on the README; a run in progress or
  cancelled never hides a red one; the Scorecard badge shows the score; a
  failed source turns the run red the same day. `readme_badges.py
  check-readme`, run as a pre-commit and pre-push hook, refuses any README
  image outside the one design, so the mix cannot come back unnoticed.
- **Negative**: the publish job holds `contents: write`, which GitHub cannot
  limit to one branch. The script writes only `refs/heads/badges` and never
  forces, and the master ruleset requires a pull request, so the token cannot
  move `master`. An SVG cannot age by itself: if the workflow stops, the
  badges keep their last values until someone sees the missing run. The six
  hours count from the read time `state.json` records. A run with unchanged
  SVGs commits once a badge's new read time is an hour or more newer than the
  recorded one, so the recorded time trails the last successful read by less
  than an hour and `stale` can come up to an hour early; committing on every
  run would quadruple the branch's growth.
- **Neutral / follow-ups**: issue 3394 tracks publishing with a dedicated
  GitHub App token instead of the workflow token, plus a second ruleset on
  `refs/heads/badges`, managed by `scripts/dev/check_repository_security.py`,
  that lets only that App write the branch. A ruleset that only refuses
  deletion and non-fast-forward updates would still let any writer add
  commits, so it is not a follow-up on its own. The Praetor governance badge
  in the `praetor:readme-governance` block keeps GitHub's design until
  Praetor offers the style (Praetor issue 1893), and `check-readme` then
  drops its one exception. This style is the reference for the ecosystem
  README badge audit.

## References

- Source: `req` (operator order `vmafx-readme-badges-and-lint-red-20261110`,
  2026-11-10, paraphrased): every README badge in one design, and no red
  default branch anywhere in the fleet.
- [Research-3519](../research/3519-readme-badges-rendered-branch.md): the
  sources, measurements and alternatives behind this decision.
- Controller answer `vmafx-readme-badge-style-r1` (2026-11-10, option b,
  paraphrased): one `for-the-badge` design everywhere, a workflow rendering the
  dynamic badges with a pinned `badge-maker` onto a `badges` branch with the
  least rights, a stale marker after six hours; it answers the operator order
  `vmafx-readme-badges-and-lint-red-20261110` (paraphrased: the README badge
  row mixes designs).
- Operator answer `vmafx-badges-app-token-ruleset` (2026-11-10, option a,
  paraphrased): land on the workflow token now, and harden the `badges`
  branch with a GitHub App token and its own ruleset as a follow-up
  (issue 3394).
- [ADR-3366](3366-readme-presentation-standard.md): the README presentation
  standard.
- shields.io GitHub Actions workflow status service:
  <https://github.com/badges/shields/blob/master/services/github/github-actions-workflow-status.service.js>.
- `badge-maker` package: <https://www.npmjs.com/package/badge-maker>.
- GitHub Actions `workflow_run` event:
  <https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#workflow_run>.
