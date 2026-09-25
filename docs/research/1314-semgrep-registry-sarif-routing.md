<!-- markdownlint-disable MD013 MD060 -->

# Research-1314: Semgrep registry SARIF routing

- **Status**: Active
- **Workstream**: [ADR-1314](../adr/1314-semgrep-registry-advisory-artifact.md)
- **Last updated**: 2026-09-25
- **Scope**: `.github/workflows/security-scans.yml` Semgrep authority boundary
- **Outcome**: keep local rules blocking; retain registry-pack output only as an artifact

## Finding

The workflow described `p/cwe-top-25`, `p/c`, and `p/python` as advisory and
ran that step with `continue-on-error: true`, but then uploaded
`semgrep-registry.sarif` through `github/codeql-action/upload-sarif`. GitHub
groups that upload and the repository-owned `semgrep-local` upload under the
same `Semgrep OSS` check. ADR-1297 requires that check, so registry findings
were merge-blocking despite the comment and step setting.

This is an authority-routing defect, not a reason to suppress findings. The
local `.semgrep.yml` is version controlled, reviewed, run with `--error`, and
remains uploaded to Code Scanning. Registry packs are moving discovery inputs;
their fetch and contents can change without a commit in VMAFx.

## Options tested against the contract

| Route | Local findings block | Registry findings retained | Registry churn can block |
|---|---:|---:|---:|
| Upload both categories | yes | yes, Security tab | yes |
| Remove registry scan | yes | no | no |
| Separate code-scanning configuration | yes | yes, Security tab | not mechanically provable with the current shared tool/check identity |
| Upload registry SARIF as an artifact | yes | yes, 14 days | no |

The artifact route is the smallest boundary that is visible in source and can
be mutation-tested. It reuses the repository's existing SHA-pinned
`actions/upload-artifact` v7.0.1 action and introduces no dependency or network
fetch beyond the already-running registry scan.

## Regression contract

`SecurityWorkflowContractTest.test_semgrep_registry_results_stay_advisory`
fails on the previous workflow because it finds `category: semgrep-registry`
and no advisory artifact step. It requires all of these together:

- `.semgrep.yml` still runs and uploads as `semgrep-local`;
- `Semgrep OSS` remains marked for the required aggregator;
- the registry scan remains explicitly advisory and produces SARIF;
- no `semgrep-registry` Code Scanning category exists; and
- the registry SARIF is archived with the pinned artifact action.

Focused replay:

```bash
python3 -m unittest scripts.ci.test_security_workflow_contract
python3 -m unittest scripts.ci.test_fail_closed_ci
bash scripts/ci/check-aggregator-names.sh
```

## Rebase note

If upstream or a later refactor combines the two scans, preserve the authority
split by destination: repository-owned local-rule SARIF may enter Code
Scanning; unpinned registry-pack SARIF must remain an ordinary advisory
artifact unless a later ADR defines a reproducibly pinned policy.
