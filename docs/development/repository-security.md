# Repository security enforcement

VMAFx protects `master` with the active **VMAFx master security** repository
ruleset. Its desired settings live in
[repository-security-policy.json](../../.github/repository-security-policy.json).
The existing classic branch protection remains in place.

## Merge requirements

Every merge needs one independent human approval after the latest push, resolved
review threads, and an up-to-date branch with the **Required Checks Aggregator**
check passing from GitHub Actions. New commits dismiss stale approvals. Only
squash and rebase merges are allowed; history stays linear. Force pushes and
branch deletion are blocked. AI reviews do not satisfy the human approval
requirement.

### The single bypass actor

The ruleset carries **exactly one** bypass actor: the `lusoris` account, by
numeric user id, declared in `.github/repository-security-policy.json`.

What it waives:

- The bypass waives the approval requirement, not the tests: `Required Checks
  Aggregator` is still a required status check.
- No role tier, bot or second account holds bypass, and the drift checker fails
  on any actor that is not the declared one.

Why it exists: this is a single-maintainer repository. GitHub forbids approving
your own pull request, so with no bypass actor the approval requirement had
nobody who could satisfy it and nothing merged for a week
([ADR-1252](../adr/1252-solo-maintainer-declared-bypass.md), superseding
[ADR-1248](../adr/1248-repository-security-enforcement.md)).

!!! note
    Remove the bypass as soon as a second maintainer can review.

#### Bypass mode: pull request only

The standard for a single-maintainer repository is bypass mode `pull_request`:
the maintainer merges a pull request without an approval and cannot push to
`master` outside one. The policy file declares that mode, and the ruleset asks
for no approval, so a pull request lands at a clean merge state once its
required checks pass
([ADR-3313](../adr/3313-solo-ruleset-zero-reviews.md), superseding
[ADR-3273](../adr/3273-solo-bypass-mode-dated-exception.md)). Generated files
(changelog, ADR index, rebase notes) land through a render pull request, not by
a direct push
([ADR-3295](../adr/3295-render-only-pull-request.md)).

The exception list holds no entry for the bypass mode. The security checker
fails when the policy declares any other mode and the list has no unexpired
entry of rule `solo-repository-bypass-mode` for the policy file, and it fails
when such an entry is still listed while every mode is `pull_request`. This
part needs no network:

```bash
python3 -B scripts/dev/tests/test_repository_security.py
python3 scripts/ci/lint_exceptions.py check
```

## One declaration of the ruleset

The [policy file](../../.github/repository-security-policy.json) is the only
declaration of the ruleset, and the drift checker compares it with the live one.
It declares:

- no required approval and no code-owner review
  ([ADR-3313](../adr/3313-solo-ruleset-zero-reviews.md));
- no last-push approval;
- the `Required Checks Aggregator` check;
- no required signatures;
- the single declared bypass actor.

praetor's own ruleset template (`.github/rulesets/main.json`) asks for two
approvals, code-owner review and signed commits, and cannot express the live
values. `.standards.yaml` therefore declines it
(`adoption.decline: [branch-ruleset]`) and the file is absent. A repository
must not look stricter than it is
([ADR-1252](../adr/1252-solo-maintainer-declared-bypass.md),
[ADR-1504](../adr/1504-decline-praetor-branch-ruleset.md)).
`scripts/dev/tests/test_repository_security.py` fails when the template
returns, the decline goes, or a declared review value differs from the live
one.

These settings supplement the local lint/test and hosted gate requirements in
[agent hard rules](agent-hard-rules.md). They do not resume the merge train or
establish that
a release is ready. Scorecard's historical review results change only as new
reviewed work lands; enabling protection cannot rewrite old review history.

Private vulnerability reporting is enabled. Use the confidential route
documented
in [SECURITY.md](../../SECURITY.md); the setting alone is not evidence of
response
times or past remediation.

## Check for drift

From a checkout with Python 3 and GitHub CLI installed:

```sh
python3 scripts/dev/check_repository_security.py
python3 -m unittest discover -s scripts/dev/tests -p test_repository_security.py
```

The checker reads only GitHub metadata and returns nonzero for configuration
drift, incomplete responses, or API failures. It verifies the named ruleset and
its effective rules on `master`, including the origin app for the required
check.
Unrelated rulesets are retained.

Its behaviour in detail:

- **Report.** An optional `--report /tmp/security-readback.json` writes policy,
  errors and API evidence to a new file; an existing report is never
  overwritten.
- **REST host.** Requests use the fixed HTTPS host `api.github.com` with no
  redirects.
- **Token.** When `GH_TOKEN` is set, it is used for those reads. CI uses its
  existing ephemeral GitHub token to avoid the anonymous shared-IP rate limit;
  no administration permission or stored personal access token is required. Do
  not print tokens.
- **Bypass actors.** GitHub omits `bypass_actors` from REST responses for
  readers
  without ruleset write access. The checker then uses `gh api graphql` to read
  only the matching ruleset's bypass actor count and compares it with the number
  of actors the policy declares. Use an existing `gh auth login` session
  locally, or `GH_TOKEN` in CI. A missing or inaccessible count fails the
  check: omission never means a match.
- **Limit of the count path.** A count cannot tell one actor from another, so a
  reader without admin rights cannot confirm that the single bypass actor is the
  declared one. A reader with admin rights sees the list and the checker
  compares identities exactly.
- **Timeouts.** REST requests and the CLI subprocess each have a 30-second
  timeout.

## Maintain the policy

Change the checked-in policy through review first. A repository administrator
then extracts its `ruleset` object and applies that reviewed JSON through
GitHub's
[ruleset API](https://docs.github.com/en/rest/repos/rules). Use the existing
ruleset's ID for an update; do not create a duplicate or delete classic
protection.
The initial managed ruleset ID was `22587111` (verified on 2026-09-08).
Discover and read back the current ID and payload before applying an update.

Run the checker after applying the reviewed payload and retain the readback
with the change. The checker deliberately has no apply mode. CI has no authority
to modify protection. If a check fails, restore the intended setting or review a
policy change; do not convert the gate to an advisory success.

See [ADR-1248](../adr/1248-repository-security-enforcement.md) and
[the verification digest](../research/2057-repository-security-enforcement.md).
