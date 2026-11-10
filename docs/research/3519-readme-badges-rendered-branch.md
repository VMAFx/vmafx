<!-- markdownlint-disable MD013 MD060 -->
# Research-3519: README status badges rendered in one style onto a `badges` branch

- **Status**: Completed
- **Workstream**: [ADR-3519](../adr/3519-readme-badges-rendered-branch.md), [ADR-3366](../adr/3366-readme-presentation-standard.md)
- **Last updated**: 2026-11-10

## Question

How can the ten README status badges (seven workflow badges, OpenSSF Scorecard, OpenSSF Best Practices and Codacy) share the one `for-the-badge` design that [ADR-3366](../adr/3366-readme-presentation-standard.md) sets, report the run this project chooses, and be published with the fewest rights a workflow can hold?

## Sources

- [`services/github/github-actions-workflow-status.service.js`](https://github.com/badges/shields/blob/master/services/github/github-actions-workflow-status.service.js) in the shields repository (read 2026-11-10): the shields.io workflow badge fetches GitHub's own `badge.svg` and reads its text.
- GitHub REST API, ["List workflow runs for a workflow"](https://docs.github.com/en/rest/actions/workflow-runs?apiVersion=2022-11-28#list-workflow-runs-for-a-workflow) (`GET /repos/{owner}/{repo}/actions/workflows/{workflow_id}/runs`): `branch`, `event`, `status` and `per_page` filters; default page size 30.
- Ruleset bypass actors, from ["Creating rulesets for a repository"](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/creating-rulesets-for-a-repository#granting-bypass-permissions-for-your-branch-or-tag-ruleset), section "Granting bypass permissions for your branch or tag ruleset" (GitHub Docs, read 2026-11-10): the eligible bypass actors are repository admins and owners, the maintain or write role, teams, GitHub Apps and Dependabot.
- Bot commit signing, from ["About commit signature verification"](https://docs.github.com/en/authentication/managing-commit-signature-verification/about-commit-signature-verification#signature-verification-for-bots), section "Signature verification for bots" (GitHub Docs): GitHub signs commits a GitHub App or the workflow token creates through the API when no custom author or committer is set.
- Commit [`e3d3cbcb602af571f93a1cc72e71ed71fad14b38`](https://github.com/VMAFx/vmafx/commit/e3d3cbcb602af571f93a1cc72e71ed71fad14b38) in this repository (an API commit by a GitHub App bot): committer `web-flow`, signature verified.
- [gh-aw issue 73207](https://github.com/github/gh-aw/issues/73207) (opened 2026-10-30) and the [Codex Review Gate marketplace page](https://github.com/marketplace/actions/codex-review-gate) (read 2026-11-10): the workflow token cannot be added as a ruleset bypass actor on its own; integration id 15368 names the whole GitHub Actions App, not one workflow.
- Scheduled runs, from ["Events that trigger workflows", section `schedule`](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#schedule) (GitHub Docs, read 2026-11-10): a scheduled run can start late when Actions is under load, most often at the start of an hour, and queued jobs can be dropped; scheduled workflows in a public repository are disabled after 60 days without activity.
- zizmor audit [`dangerous-triggers`](https://docs.zizmor.sh/audits/#dangerous-triggers): rates `workflow_run` and `pull_request_target` as dangerous triggers.
- [docs/principles.md](../principles.md) section 6 and [ADR-1247](../adr/1247-scorecard-exact-head-gates.md): unrounded Scorecard ≥ 8.5 required on master.
- [`badge-maker` 6.0.0](https://www.npmjs.com/package/badge-maker/v/6.0.0), the shields.io renderer, pinned in `tools/badges/package-lock.json`.
- `scripts/ci/readme_badges.py`, `.github/workflows/readme-badges.yml`, `scripts/ci/check_repository_security.py` and `.github/repository-security-policy.json` on the pull request 3393 branch.

## Findings

1. **GitHub's badge has one fixed design, and shields.io reads it.** `badge.svg` cannot be restyled. The shields.io workflow badge takes its text from that same image, so a `for-the-badge` shields URL changes the look but not which run the badge reports. Rendering the SVGs from the API is the only path that gives the project both the design and the run choice.

2. **Run choice.** GitHub's badge shows the latest run, so a cancelled or skipped run hides a failed one. `readme_badges.py` lists completed push runs on the default branch (`per_page` = `RUNS_PER_PAGE` = 30) and takes the newest one with a conclusion other than cancelled or skipped (`PASSED_OVER`). `success` reads passing; `failure`, `timed_out` and `startup_failure` read failing; any other conclusion reads `unknown`. An older run never stands in for a newer decisive one. When all 30 runs are passed over, the badge reads `unknown` rather than paging further back.

3. **Trigger.** A `workflow_run` trigger would refresh a badge within a minute of a master run, but zizmor rates it a dangerous trigger, and its `branches: [master]` filter also matches a fork's branch named `master`. A schedule (`7,22,37,52 * * * *`) with a default-branch guard on the render job and checkouts at `github.sha` has no outside input. Fifteen minutes is the intended step, not a bound on a badge's age: GitHub can start a scheduled run late or drop it when Actions is under load, and the minutes 7, 22, 37 and 52 stay clear of the busy start of the hour.

4. **Rights.** `contents: write` cannot be limited to one branch. The master ruleset requires a pull request and the workflow token holds no bypass, so the token cannot move `master`. The token also cannot be named as a ruleset bypass actor: the eligible actors are roles, teams, GitHub Apps and Dependabot, and the GitHub Actions App (integration 15368) covers every workflow in the repository. A `badges` ruleset that refuses deletion and non-fast-forward pushes therefore still lets any workflow with `contents: write` add forward commits; refusing every other writer needs a dedicated GitHub App as the only bypass actor.

5. **Signed commits.** Commits the workflow token creates through the Git data API with no custom author or committer are signed by GitHub and show as verified (committer `web-flow`). The publish job sets no author or committer and moves the branch only to a commit GitHub reports as verified, so every commit the publish job adds is verified without any signing key in the workflow. That covers the publish job's commits, not the whole branch: until issue 3394 lands, another writer with `contents: write` can add commits of its own (finding 4).

6. **Staleness.** A source that fails turns that run red at once: its `verdict` step exits 1. The badge itself keeps the last value for `STALE_AFTER` = 6 hours and then reads `stale`. The publish job skips the commit while every SVG matches the branch and the branch's last commit is younger than `QUIET_WINDOW` = 1 hour, so with runs on schedule the read time `state.json` records can lag the true last read by up to 1 hour plus one 15-minute step: `stale` can come up to 75 minutes early, and one schedule step late plus any delay GitHub adds to a scheduled run. Committing `state.json` on every run would remove that lag at the cost of about four commits an hour instead of one.

7. **Scorecard threshold.** The badge is green from an unrounded 8.5, the gate [docs/principles.md](../principles.md) and [ADR-1247](../adr/1247-scorecard-exact-head-gates.md) set for master. A score of 8.46 shows `8.5 / 10` in amber, so the rounded text never hides a miss.

8. **Pinned renderer.** `badge-maker` 6.0.0 draws the same SVG shields.io serves, without a request to shields.io per page view. The lock file pins it with its integrity hash.

## Alternatives explored

- **`workflow_run` trigger**: Rejected. Faster badges, but a dangerous trigger with a branch filter that fork branches can match.
- **shields.io dynamic URLs with `style=for-the-badge`**: Rejected. Every page view depends on shields.io, and the workflow badge keeps GitHub's run choice.
- **Each service's own badge**: Rejected. Four designs in one block and a Scorecard badge without a score.
- **GitHub Pages files**: Rejected. Pages deploys the documentation site; a second deploy path for ten files is not worth it.
- **Commit `state.json` on every run**: Rejected. Removes the 75-minute early `stale`, but quadruples the branch's growth.
- **Rendered SVGs on a `badges` branch, schedule trigger, workflow token**: Chosen.

## Open questions

- Publishing with a dedicated GitHub App token plus a second managed ruleset on `badges`, so that no other workflow can write the branch. `check_repository_security.py` manages one ruleset today; a second one needs a policy change and an App installation. Tracked in issue 3394.
- The Praetor governance badge keeps GitHub's design until Praetor offers the style (Praetor issue 1893).

## Related

- [ADR-3519](../adr/3519-readme-badges-rendered-branch.md) — Render the README status badges in one style onto a `badges` branch
- [ADR-3366](../adr/3366-readme-presentation-standard.md) — README presentation standard
- [README badges](../development/readme-badges.md)
