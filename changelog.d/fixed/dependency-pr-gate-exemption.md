### Fixed

- `.github/workflows/rule-enforcement.yml`: exempt strictly dependency-only bot pull
  requests (Renovate and Dependabot) from the Doc-Substance Gate (ADR-0100 / ADR-0167)
  and the Deep-Dive Deliverables Checklist (ADR-0108). Eliminates persistent
  false-positive red badges across routine dependency bumps (ADR-1152).
- `scripts/ci/classify-dependency-pr.sh`: add shared classifier script that gates
  exemption on both bot identity (author/branch) and an explicit, conservative
  allowlist of dependency manifests and lockfiles, ensuring PRs touching source
  code remain subject to documentation checks.
