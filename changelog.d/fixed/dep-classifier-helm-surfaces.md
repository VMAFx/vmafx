- **The dependency-only PR classifier missed the Helm chart surface, so
  Renovate image-tag bumps were blocked by the deliverables gate
  (ADR-1152).** `scripts/ci/classify-dependency-pr.sh` requires that *every*
  changed path be an allowed dependency manifest. Renovate's helm-values and
  docker-image managers pin container tags inside `deploy/helm/*/values.yaml`
  and the chart templates, and neither matched any allowlist entry -- so
  condition (b) failed even though the author and branch checks passed.
  Across 25 consecutive Renovate PRs these were the *only* two paths the
  allowlist missed (`deploy/helm/vmafx/values.yaml` and
  `deploy/helm/vmafx/templates/tests/test-connection.yaml`); everything else
  already matched via `pyproject.toml`, `docker/*`, `Dockerfile*`, `go.mod` or
  `.github/workflows/*`. PR #1232 was failing `Deep-Dive Deliverables
  Checklist (ADR-0108)` and the Required Checks Aggregator for exactly this
  reason, with no way for a bot to satisfy a checklist it cannot write.

  The allowlist now covers `deploy/helm/*` plus the `Chart.yaml` /
  `Chart.lock` and `docker-compose*.yml` / `compose*.yml` basenames.
  Whole-prefix granularity matches how `docker/*` and `.github/workflows/*`
  are already treated, and the gate is **not** weakened: exemption still
  requires condition (a) (bot author or bot branch), so a human editing the
  same chart is fully gated, and a bot PR that also edits source stays gated.
  Both of those are now regression-tested -- `Helm values + core/src/model.c`
  and a human-authored change to the same Helm path must both come back NOT
  exempt. Suite is 19/19.
