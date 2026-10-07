---
paths:
  - scripts/ci/test_e2e_runtime_contract.py
  - scripts/ci/check-helm-selector-isolation.py
  - scripts/ci/tests/test_check_helm_selector_isolation.py
  - scripts/ci/test_security_workflow_contract.py
  - scripts/ci/tests/test_master_concurrency_contract.py
  - scripts/ci/tests/test-dedupe-gate.sh
invariant: Contract test and caller change together.
---
<!-- markdownlint-disable MD013 MD060 -->
# Workflow contract tests: E2E, Helm selectors, Security Scans, dedupe gate

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `test_e2e_runtime_contract.py` | `rule-enforcement.yml` and `e2e-k8s.yml` — `Verify E2E runtime contract` | always-on PR gate and exact E2E lane enforce explicit CPU node + Go server targets, three-image transfer into kind, exact-local Helm pulls, and real chart-backed scoring case. Keep it outside E2E trigger gate as well as inside image job. |
| `check-helm-selector-isolation.py` | `helm-chart.yml` — `Workload selector isolation` | Positional `helm template` output files; exit 0 isolated, 1 overlap or unevaluable selector, 2 unreadable input. Step runs `tests/test_check_helm_selector_isolation.py` first, then renders Deployment, StatefulSet, Job workloads with operator, node, PDBs on (ADR-1353). |

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `test_security_workflow_contract.py` | `rule-enforcement.yml` — `Verify Security Scans concurrency contract` | Security Scans group must include workflow, event name, and ref. This keeps same-event cancellation while preventing schedule on `refs/heads/master` from canceling master-push CodeQL run (or vice versa). same test pins C/C++ Meson configure before CodeQL initialization, compile after initialization, and external `${{ runner.temp }}/build` root so generated compiler probes are never extracted as repository source. |

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `tests/test-dedupe-gate.sh` | `standards-gate.yml` — `Reject duplicate implementation families`; `rule-enforcement.yml` — `Verify duplicate implementation gate`; `.pre-commit-config.yaml` — `dedupe-gate-contract`; `lefthook.yml`; `make verify-all` | clone scan stays explicit in required Standards job, both blocking local lefthook stages, and aggregate local command. Its real-Make fixture proves scanner failure makes `make verify-all` fail. `standardsctl audit` is not substitute because it does not run AST clone detector. |

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `tests/test_master_concurrency_contract.py` | every workflow triggered by push to `master` (`.github/workflows/*.yml`); `.pre-commit-config.yaml` — `test-master-concurrency-contract` | master push must not cancel or evict earlier master run: concurrency group carries `github.sha` on master, `cancel-in-progress` stays for PR refs, and block that must serialise is listed in test with reason (ADR-1673). Stale list entries fail. |
