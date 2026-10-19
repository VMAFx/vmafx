<!-- markdownlint-disable MD013 MD060 -->
# ADR-2169: CI runs the tier a pull request owes, not the whole suite on every push

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: lusoris
- **Tags**: `ci`, `renovate`, `release`, `agents`

## Context

Over the 24 hours before this decision the repository ran more than 1000
workflow runs. By pull-request class (runs and run minutes, wall time):

| Class | Runs | Minutes |
|---|---:|---:|
| Own pull requests | 284 | 7414 |
| Release-please pull request | 157 | 5174 |
| Master push | 120 | 3876 |
| Renovate | 133 | 2752 |
| Draft pull requests (rc4) | 144 | 1978 |
| Pushes to other branches | 104 | 1125 |

Every one of them ran, or started, the whole suite: the platform build matrix,
the GPU and container lanes, coverage, the FFmpeg lanes. The release pull
request is refreshed on every merge to master and re-ran all of it for a diff of
version markers. Renovate opened a pull request per dependency. A draft pull
request still started jobs in workflows that carry no draft gate. The 104 pushes
to other branches are the two workflows praetor ships with an unfiltered `push:`
trigger.

The maintainer decided (Q-070, 2026-10-07) to run the full suite only where it
decides something, and to say in one place what each kind of event owes.

## Decision

We will route CI by **tier**. One definition, `.github/ci-tier.json`, says which
required contexts each tier owes; `scripts/ci/ci_tier.py` decides the tier of an
event; `.github/workflows/ci-tier.yml` (a reusable workflow) hands the decision
to every workflow as the first job `tier`; the Required Checks Aggregator reads
the same file.

| Event | Tier | Runs |
|---|---|---|
| Fork pull request | full | everything |
| Own pull request (head repository is this repository, Renovate included) | light | lint, format, the fast suite, the governance gates; not the `full_only` contexts |
| Own pull request labelled `ci: full` | full | everything |
| Release pull request (bot author, or the maintainer account with a release-only diff; `release-pr-exempt.sh`) | release-light | `Release Script Contract` (and the praetor gates, which this repository cannot gate) |
| Release pull request labelled `autorelease: cut` | full | everything |
| Draft pull request | none | nothing but the declared untiered jobs |
| Master push, dispatch, schedule | full | everything |
| Push to another branch or a tag | none | nothing (two praetor-locked exceptions, below) |

- **Light-tier jobs** gate on `needs.tier.outputs.light`, **full-tier jobs** on
  `needs.tier.outputs.full`; the tier job carries the draft gate for the whole
  workflow. The full-only contexts are the platform legs of the build matrix
  (the two `+DNN` Ubuntu legs stay light), the all-backend `Build` lanes,
  coverage and sanitizers, the dev container, docker image, FFmpeg, tester and
  release dry-run lanes, and the self-hosted hardware lanes.
- **The aggregator** takes `CI_TIER`, `always` and `full_only` from the same file.
  A context the tier does not owe is accepted when absent or skipped and still
  fails when it ran and failed. A failed tier job fails the aggregator, so a tier
  that could not be decided cannot read as a pass. GitHub creates the check runs
  of the jobs that need the `tier` job only once it has completed, so the
  aggregator keeps waiting while any workflow's tier decision (named
  `CI tier (<workflow>) / Decide the CI tier`) is queued, running or just done;
  without that, a saturated queue made every light-tier context read as never
  reported (observed on the first pull request). A tier that is not named owes
  the whole list, which is the behaviour before this ADR.
- **No workflow listens for `labeled`.** `ci-escalate.yml` does, and for the two
  escalation labels only: it re-runs the latest run of every pull-request workflow
  on the head commit (cancelling a running one first), and each re-run decides its
  tier again from the live labels. A listener in every workflow would start one
  run per workflow for every label anybody applies, and a skipped aggregator run
  on the same commit would read as a passing required context. This differs from
  the instruction to add `labeled` to every trigger; the effect (labelling starts
  the full suite) is the same.
- **Renovate**: the weekly window (Monday, before 6am Europe/Vienna) is the
  default schedule, a first catch-all rule groups every minor, patch, digest and
  pin update into one pull request, the specific rules keep their groups and
  settings, `rebaseWhen` is `conflicted`, and `vulnerabilityAlerts` still opens
  security updates at any time.
- **Drafts**: every workflow with a `pull_request` trigger gates on
  `draft == false` through the tier job and listens for `ready_for_review`. Before
  this change these lacked the gate: `standards-gate`, `scorecard-policy`,
  `docs`, `doxygen-public-api`, `helm-chart`, `rust-ci`, the `impact` planner of
  every planner workflow (`build`, `dev-container-build`, `docker-image`,
  `ffmpeg-integration`, `docker-publish-tester`, `windows-tester-bundle`),
  `tidy-metal` (and `ready_for_review`) and `e2e-k8s` (and `ready_for_review`).
- **Branch triggers**: every `push:` trigger names `master`. The exceptions are
  `praetor-api.yml` and `praetor-docs.yml`, which `praetorctl audit` locks byte
  for byte and which carry an unfiltered `push:` and no draft gate; they are
  declared with a reason and an expiry in `ci-tier.json` and reported upstream.
- **Master push** gains `Sanitizers ASan+UBSan` (it ran on pull requests only),
  because the full-only contexts have to run somewhere.
- `pr-type-label.yml` no longer runs on `synchronize` (the label derives from the
  title, which a push does not change); `e2e-k8s.yml` starts a job only for a ready
  pull request that carries its opt-in label.

The contract is executable. `scripts/ci/tests/test_ci_routing_contract.py` builds
synthetic events (own, fork, Renovate, release with and without the cut label, a
person on a release branch name, draft, master push, feature-branch push, tag,
labels) and, with `scripts/ci/ci_router.py` and
`scripts/ci/ci_expressions.py`, works out which jobs of the real workflows run.
Run against master's workflows it fails 14 of its 22 cases (the cases whose
behaviour this ADR changes); it also plants six defects in a copy of the workflows
and requires each to be caught.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `if:` lines per job with a copied expression | No new job | 30 independent copies of one rule; nothing proves they agree with the aggregator | The brief asks for one definition |
| Trigger-level filters (`branches`, `paths`) | No run at all | They cannot see the head repository, the draft flag or a label, and a required context that never starts blocks the merge (BUG-098) | Cannot express the tiers |
| `labeled` in every workflow's `types` | The instruction as written | One run per workflow per label event; a skipped aggregator run on the same commit reads as a pass | Replaced by one escalation workflow |
| `workflow_dispatch` the full workflows on a label | Re-runs only the full-tier jobs | Dispatched runs carry another event, so the aggregator would need a second reporter for the same required name | Re-running the originals keeps one reporter per name |
| Read the tier from the event payload only | No API call | A re-run keeps the labels of the original event, so the escalation would not change the tier | Labels are read live; the payload is the stated fallback |
| Keep Renovate as it was and rely on `prConcurrentLimit` | No config change | A dozen light-tier pull requests per day each run lint and the fast suite | Grouping removes the fan-out |

## Consequences

- **Positive**: an estimate from the 24-hour data (wall minutes; a workflow run is
  counted when it starts, whatever its jobs do): own pull requests 7414 to about
  3500, the release pull request 5174 to about 170 per refresh set, drafts 1978
  to about 500 (the two praetor gates), Renovate 2752 to about 200 per day with
  weekly grouping, about 55 percent fewer run minutes overall, about 17 percent
  fewer runs (the e2e and label listeners, Renovate grouping). The aggregator of a
  light pull request stops waiting for the slowest full-tier lane (54 minutes of
  coverage) and waits for the slowest light-tier one (22).
- **Negative**: a light-tier pull request does not run the platform, GPU, coverage
  and container lanes; a break there is found on the master push, and the push
  lands through the merge train first. `ci: full` is the manual escape hatch. A
  workflow still starts for a draft, so the run count of drafts is unchanged;
  only their minutes go. The two praetor workflows keep starting on every push and
  every draft until praetor changes them.
- **Neutral / follow-ups**: the tier definition is a CI-authority file (the
  planner treats a change to it as a full plan). A required context added to the
  aggregator needs a place in `always` or `full_only`, or it is a light context;
  `test_ci_routing_contract.py` fails when the two disagree with the workflows.

## References

- `Q-070` (maintainer popup, 2026-10-07): release pull request light until
  `autorelease: cut`, own pull requests light, full on master and for forks,
  Renovate grouped and scheduled, every workflow draft-gated, push triggers only
  `master` and tags.
- [ADR-0331](0331-skip-ci-on-draft-prs.md) (draft gate), [ADR-0313](0313-ci-required-checks-aggregator.md),
  [ADR-1140](1140-ci-impact-planner.md), [ADR-1151](1151-vmafx-first-release-1-0-0.md),
  [ADR-1388](1388-release-pat-mode-gate-exemption.md).
