---
paths:
  - .github/workflows/release-dry-run.yml
  - scripts/ci/release-*.sh
invariant: Mirrors release builds; publishes nothing.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# Release dry run (ADR-1595)

- `release-dry-run.yml` runs production, operator / server / node image
  builds (linux/amd64, no push), GPU toolkit builds (no load) and
  `vmaf-mcp` wheel, sdist and SBOMs on pull requests that touch their inputs and
  weekly. `release-dry-run-plan.sh` decides groups; every path it names must
  exist (`test_plan_script_names_only_files_that_exist`).
- It holds no credential: no registry login, push, signature, attestation,
  environment or `id-token`. `test_is_a_dry_run` fails on any of them;
  step that needs one belongs to release workflow.
- `verify-mcp-sbom.sh` is one SBOM check: `supply-chain.yml` (job `sbom`)
  and dry run both call it, and `scripts/release/tests/test-verify-mcp-sbom.sh`
  plants one defect at time. Do not inline its `jq` back into workflow.
- tester image and Windows zip workflows take `pull_request` trigger with
  paths of their `push` trigger; their `validate` job narrows matrix on pull
  request (amd64; x64) and never sets `publish=true` for it. macOS bundle
  runs weekly. `test_pr_time_verify_workflows.py` executes `validate` steps.
