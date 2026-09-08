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
branch deletion are blocked. The ruleset has no administrator, bot, or other
bypass actors. AI reviews do not satisfy the human approval requirement.

These settings supplement the local lint/test and hosted gate requirements in
[AGENTS.md](../../AGENTS.md). They do not resume the merge train or establish that
a release is ready. Scorecard's historical review results change only as new
reviewed work lands; enabling protection cannot rewrite old review history.

Private vulnerability reporting is enabled. Use the confidential route documented
in [SECURITY.md](../../SECURITY.md); the setting alone is not evidence of response
times or past remediation.

## Check for drift

From a checkout with Python 3 and GitHub CLI installed:

```sh
python3 scripts/dev/check_repository_security.py
python3 -m unittest discover -s scripts/dev/tests -p test_repository_security.py
```

The checker reads only GitHub metadata and returns nonzero for configuration
drift, incomplete responses, or API failures. It verifies the named ruleset and
its effective rules on `master`, including the origin app for the required check.
Unrelated rulesets are retained. An optional `--report /tmp/security-readback.json`
writes policy, errors, and API evidence to a new file; an existing report is never
overwritten.

REST requests use the fixed HTTPS host `api.github.com` with no redirects. When
`GH_TOKEN` is set, it is used for those reads. CI uses its existing ephemeral
GitHub token to avoid the anonymous shared-IP rate limit; no administration
permission or stored personal access token is required. Do not print tokens.

GitHub omits `bypass_actors` from REST responses for readers without ruleset write
access. The checker then uses `gh api graphql` to read only the matching ruleset's
bypass actor count. Use an existing `gh auth login` session locally, or `GH_TOKEN`
in CI. A missing or inaccessible count fails the check: omission never means
zero. REST requests and the CLI subprocess each have a 30-second timeout.

## Maintain the policy

Change the checked-in policy through review first. A repository administrator
then extracts its `ruleset` object and applies that reviewed JSON through GitHub's
[ruleset API](https://docs.github.com/en/rest/repos/rules). Use the existing
ruleset's ID for an update; do not create a duplicate or delete classic protection.
The initial managed ruleset ID was `22587111` (verified on 2026-09-08).
Discover and read back the current ID and payload before applying an update.

Run the checker after applying the reviewed payload and retain the readback
with the change. The checker deliberately has no apply mode. CI has no authority
to modify protection. If a check fails, restore the intended setting or review a
policy change; do not convert the gate to an advisory success.

See [ADR-1248](../adr/1248-repository-security-enforcement.md) and
[the verification digest](../research/2057-repository-security-enforcement.md).
