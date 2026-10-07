<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-2168: Four long Linux jobs of master pushes run on Depot runners, behind a repository variable

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: lusoris
- **Tags**: `ci`, `infrastructure`, `fork-local`

## Context

A master push runs about 90 Linux jobs and 700 job-minutes on GitHub-hosted
runners. The merge train lands batches, so master pushes arrive about every 25
minutes while a batch lands, and the hosted queue saturates. The four longest
Linux jobs of a push, measured over ten master pushes of 2026-10-06 and
2026-10-07 (job duration from the Actions API, median), are `Coverage Gate`
(51.6 min), `Dev Container Build work` (49.3), `Docker Image Build work` (28.5)
and `FFmpeg SYCL work` (27.3). The `Required Checks Aggregator` (73 min) only
polls and the Windows jobs are not Linux, so neither moves.

Depot (depot.dev) runs GitHub Actions jobs on its own ephemeral runners
selected by a `runs-on` label (`depot-ubuntu-24.04[-N]`). The Developer plan
is USD 20 per month with 2,000 included minutes and USD 0.006 per further base
minute; a label's multiplier is its vCPU count over two. The maintainer's
decision is a temporary bridge until another CI provider is chosen, and the
Depot organisation carries a 10,000-minute limit that caps spending.

## Decision

We will select the runner of those four jobs with the repository variable
`VMAFX_DEPOT_LINUX_RUNNER`:

```yaml
runs-on: ${{ (github.event_name != 'pull_request' && github.ref == 'refs/heads/master' && vars.VMAFX_DEPOT_LINUX_RUNNER) || 'ubuntu-latest' }}
```

The variable holds a Depot label (for example `depot-ubuntu-24.04-8`), so the
size is chosen by the variable and not by a commit. Unset or empty means
GitHub-hosted. Pull requests, fork pull requests, other branches and every
other job stay on GitHub-hosted runners.

`scripts/ci/depot_minutes.py` sums the month's base minutes (jobs whose labels
start with `depot-`, elapsed seconds times the label multiplier, rounded up)
from the Actions API, read-only, and exits 1 at 90 % of the 10,000-minute
limit. The maintainer's local merge train calls it and clears the variable.
No workflow in the repository writes the variable, and no new credential is
added.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Variable selects the runner, a local script reports usage (this ADR) | Off by one `gh variable delete`; no secret; no commit to switch | The switch-off is as prompt as the caller | Chosen |
| Hourly guard workflow that clears the variable | Self-acting | Needs a token with variable-write permission (none is configured: the repository holds `RELEASE_BOT_TOKEN` and `SYCL_RUNNER_PROBE_TOKEN` only); a second place that decides | The Depot organisation limit already caps spending |
| Hard-code `depot-` labels | Simplest | A commit to switch back; pull requests would spend minutes | Not reversible without a PR |
| Move every Linux job | Largest queue relief | About 700 base minutes per push against 2,000 included | Cost; four jobs carry the wall-clock |

## Consequences

- **Positive**: the four jobs leave the saturated hosted queue on master
  pushes; switching off is `gh variable delete VMAFX_DEPOT_LINUX_RUNNER`.
- **Negative**: Depot images are Ubuntu 24.04 and 22.04 only; the four jobs
  ask for `ubuntu-latest`, so a job that depends on a tool of the hosted image
  can differ (compared once, see `docs/development/ci.md`). A third party runs
  the jobs. The repository must be connected to the Depot organisation by the
  maintainer.
- **Neutral / follow-ups**: temporary; remove the variable and the four
  expressions when the other CI provider is chosen.

## References

- `req` (maintainer decision Q-066, 2026-10-07, paraphrased): move the three to
  four longest Linux jobs of master pushes to Depot runners, selected by a
  repository variable; pull requests and polling jobs stay on GitHub runners.
- `req` (maintainer decision Q-067 and scope change, 2026-10-07, paraphrased):
  Depot is a temporary bridge, capped by the Depot organisation's
  10,000-minute limit; a read-only usage script replaces a guard workflow.
- Depot documentation: <https://depot.dev/docs/github-actions/runner-types>,
  checked 2026-10-07.
