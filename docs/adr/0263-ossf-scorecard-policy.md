# ADR-0263: OSSF Scorecard policy and remediation cadence

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: lusoris
- **Tags**: ci, security, supply-chain, docs

## Context

The fork ships `.github/workflows/scorecard.yml` (OpenSSF Scorecard
v2.4.3, scorecard CLI v5.3.0), which scans the repository on every
push to `master` and on a weekly cron. Results are published to the
public dashboard at `scorecard.dev/viewer/?uri=github.com/VMAFx/vmafx`
and uploaded as SARIF to the GitHub Security tab.

For an extended period the workflow has been red on every run.
Investigation (see [Research-0053](../research/0053-ossf-scorecard-investigation.md))
identifies two distinct problems that need policy answers:

1. The workflow itself fails because the `github/codeql-action/upload-sarif`
   pin (`b25d0ebf40e5...`) is an "imposter commit" — a SHA that no longer
   exists in the action's repository, which Scorecard's webapp rejects
   to defend against tag-rotation attacks. This is a one-line fix.
2. The aggregate score is **6.2 / 10**, with several checks scoring 0
   or returning `-1` (internal error). Some are addressable inside the
   fork; others are intrinsic to the project's posture (solo
   maintainer, no shipped releases yet, no OSS-Fuzz onboarding) or to
   Scorecard's tooling limits (Dockerfile-parser false positive,
   `GITHUB_TOKEN` scope on classic branch protection).

The fork needs a documented policy that says: which score checks we
treat as gates, which as informational, what target aggregate we aim
for, and how often we re-evaluate. Without this, every red Scorecard
run becomes a fresh re-investigation.

Constraints from the standing project rules:

- No tooling that requires external secrets or paid services
  (extends to user PATs stored as repo secrets).
- ADR-0108 deep-dive deliverables apply: research digest + decision
  matrix + rebase note + reproducer + CHANGELOG + state.md/rebase
  entry.
- Squash-merge + linear-history + 19 required status checks already
  enforced on master (ADR-0037).

## Decision

The fork adopts the following OSSF Scorecard policy:

1. **Target aggregate score**: ≥ 7.0 / 10 once
   the actionable items in the [Research-0053](../research/0053-ossf-scorecard-investigation.md)
   follow-up list have landed. Until then, **6.2 is the documented
   floor** while remediation is in flight.
2. **The Scorecard workflow must run green** (i.e., the workflow itself
   succeeds even when individual check scores are low). A red workflow
   masks regressions in checks that *did* score, so the build going
   red is the user-visible signal that requires human attention.
3. **Action SHAs are pinned to commits that currently exist** under the
   declared tag. Whenever a `uses:` SHA is updated (Dependabot or
   manual), the resolved SHA is recorded in a comment on the same line
   so a future "imposter commit" failure is diagnosable without log
   spelunking. Dependabot already opens grouped minor/patch PRs weekly;
   that workflow is the primary defence.
4. **Accepted score-check blockers** (will *not* be remediated; documented
   as out of scope):
   - `Code-Review` (0) — solo-maintainer artefact; squash-merging
     own PRs does not register an approval event.
   - `Branch-Protection` (-1) — `GITHUB_TOKEN` cannot read classic
     branch protection rules; resolving requires a fine-grained PAT
     stored as an org secret, which the fork's secret-management
     policy disallows. Master is in fact protected (19 required
     checks, force-push blocked, linear history); this is a tooling
     visibility gap.
   - `Maintained` (0 → will auto-resolve) — clears once the fork
     ages past 90 days from first push.
   - `CII-Best-Practices` (0) — OpenSSF Best Practices Badge
     application is a separate, multi-week external workstream;
     revisit if/when the project pursues silver/gold.
5. **Active remediation queue** (each its own follow-up PR):
   - `Vulnerabilities` (0) → bump `python/requirements.txt` lower
     bounds.
   - `Pinned-Dependencies` (-1, internal error) → upstream the
     Dockerfile-parser bug to ossf/scorecard; once fixed, run a
     hygiene sweep that SHA-pins the remaining `@vN` refs across
     the workflow tree.
   - `Fuzzing` (0) → OSS-Fuzz onboarding (separate research digest).
   - `Signed-Releases` (-1) → resolved on first `v3.x.y-lusoris.N`
     release via the existing keyless-Sigstore release-please pipeline.
   - `Packaging` (-1) → revisit if the project starts publishing to
     PyPI / a container registry tagged for Scorecard recognition.
6. **Re-evaluation cadence**: this ADR is reviewed every 90 days
   (cron-aligned with the workflow's weekly schedule × 13). The
   review checks whether any accepted blocker has shifted (e.g.,
   Scorecard upstream adds support for fine-grained-token-less
   branch-protection reading, or the Code-Review check normalises
   solo-maintainer signals).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Adopt the policy described in §Decision** (this ADR) | Documents accepted gaps once instead of re-discovering them every red run; sets a numeric target; defines remediation queue | Requires periodic re-review (90-day cadence); some accepted blockers may look like "we gave up" to outside readers | Selected — best balance of pragmatism and traceability for a solo-maintained fork. |
| Apply for an OpenSSF Best Practices Badge to lift `CII-Best-Practices` from 0 to ≥ 7 | +0.5 to +1.5 aggregate points; signals project maturity | Multi-week effort; requires sustained external attention; not aligned with current fork priorities | Deferred — tracked as a future workstream, not a blocker. |
| Add a fine-grained PAT as a repo secret to unblock `Branch-Protection` and lift -1 → 10 | +1 aggregate point; surfaces real branch-protection posture in the dashboard | Stores a personal access token as a CI secret, which is exactly the kind of standing credential the fork's secret-policy forbids; rotation burden falls on the maintainer | Rejected — score gain is cosmetic (master IS protected); the policy cost is real. |
| Disable the Scorecard workflow entirely | Removes the red-X noise from PR checks | Loses the supply-chain regression signal entirely; abandons the public-dashboard surface; conflicts with the fork's documented security-posture stance | Rejected — Scorecard's value is the *delta* signal, not the absolute score. |
| Treat every `0`-score check as a release blocker | Maximum security posture | Half the zero scores (Code-Review, Maintained, CII) are not really addressable by code change; would block all releases on artefacts the fork has no control over | Rejected as overreach. |

## Consequences

- **Positive**:
  - The Scorecard workflow goes green again on every push, which
    restores the supply-chain regression signal (a *new* check
    falling off becomes immediately visible against the green
    baseline).
  - Every red Scorecard run can now be triaged against this ADR in
    seconds instead of restarting the investigation.
  - The follow-up queue is concrete and prioritised; nothing falls
    through the cracks because of "we'll get to it eventually".
  - The dashboard at `scorecard.dev/viewer/?uri=github.com/VMAFx/vmafx`
    starts publishing fresh results again (the 400-error path was
    skipping publish on every recent run, so the live page was
    stale).
- **Negative**:
  - The 90-day re-evaluation cadence is a recurring chore. If the
    review is skipped, accepted blockers may silently drift (e.g.,
    Scorecard upstream adds support for our use case and we don't
    notice). Mitigation: the cadence is documented here and in the
    ADR README; future audits surface the staleness.
  - The accepted blockers section is candid that some 0-scores are
    structural for a solo-maintainer fork. External readers may
    misread this as low security maturity. Mitigation: the
    Research-0053 digest explains *why* each blocker is structural,
    not "we couldn't be bothered".

## References

- [Research-0053](../research/0053-ossf-scorecard-investigation.md) —
  per-check breakdown, log evidence, full remediation queue.
- ADR-0037 — master branch protection (informs the
  `Branch-Protection` accepted-blocker ruling).
- ADR-0118 — keyless Sigstore release signing (informs the
  `Signed-Releases` follow-up).
- Scorecard checks reference:
  <https://github.com/ossf/scorecard/blob/main/docs/checks.md>
- Scorecard action workflow restrictions (imposter-commit detection):
  <https://github.com/ossf/scorecard-action#workflow-restrictions>
- Public dashboard:
  <https://scorecard.dev/viewer/?uri=github.com/VMAFx/vmafx>
