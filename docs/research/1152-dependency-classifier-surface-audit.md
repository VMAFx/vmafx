# Research digest 1152 — which paths do Renovate PRs actually touch?

Supports the fix to `scripts/ci/classify-dependency-pr.sh` extending
[ADR-1152](../adr/1152-dependency-pr-gate-exemption.md).

## Question

ADR-1152 exempts dependency-only bot PRs from the documentation gates, but the
exemption is a conjunction: (a) bot author or bot branch, AND (b) *every*
changed path is an allowed dependency manifest. PR #1232 satisfied (a) and
failed (b). Rather than allowlisting the one path that PR happened to touch,
the question is: **what is the complete set of paths Renovate actually edits in
this repository**, so the allowlist can be closed rather than patched again on
the next bump?

## Method

Enumerated every changed path across the 25 most recent `app/renovate` PRs and
matched each against the classifier's two `case` blocks:

```bash
for n in $(gh pr list --repo VMAFx/vmafx --state all --author app/renovate \
             --limit 25 --json number -q '.[].number'); do
  gh pr view "$n" --repo VMAFx/vmafx --json files -q '.files[].path'
done | sort | uniq -c | sort -rn
```

## Result

| Path (by frequency) | Already matched by |
| --- | --- |
| `mcp-server/vmaf-mcp/pyproject.toml` (6) | basename `pyproject.toml` |
| `dev/Containerfile` (4) | exact path |
| `docker/Dockerfile.production-gpu` (3) | prefix `docker/*` |
| `dev-llm/pyproject.toml`, `ai/pyproject.toml` (3 each) | basename |
| `.pre-commit-config.yaml` (3) | exact path |
| `tools/*/pyproject.toml` (2+1+1) | basename |
| `docker/Dockerfile.node`, `docker/dev/*.Dockerfile` | prefix `docker/*` |
| `Dockerfile`, `Dockerfile.go-server`, `mcp-server/vmaf-mcp/Dockerfile` | basename `Dockerfile*` |
| `.github/workflows/supply-chain.yml` (2) | prefix `.github/workflows/*` |
| `renovate.json` | exact path |
| `go.mod`, `go.sum` | basename |
| **`deploy/helm/vmafx/templates/tests/test-connection.yaml`** (2) | **nothing** |
| **`deploy/helm/vmafx/values.yaml`** (1) | **nothing** |

Exactly two unmatched paths, both under `deploy/helm/`. The allowlist was not
broadly wrong — it had one missing surface.

## Why the Helm surface is a dependency surface

Renovate's helm-values and docker-image managers rewrite container image tags
inside chart `values.yaml` and inside templates that hardcode a helper image
(`test-connection.yaml` pins a `busybox`-class image for the chart test hook).
These are image pins, exactly like a `Dockerfile` `FROM` line, and carry no
user-discoverable surface change — which is the property ADR-1152's exemption
turns on.

## Alternatives considered

| Option | Verdict |
| --- | --- |
| **Allowlist `deploy/helm/*` (chosen)** | Matches the granularity already used for `docker/*` and `.github/workflows/*`. Safe because the exemption is a conjunction: condition (a) still requires a bot author or bot branch, so a human editing chart logic is fully gated, and a bot PR that also edits source is gated. |
| Allowlist only `deploy/helm/*/values.yaml` | Would have re-blocked on the template path, which the audit shows Renovate edits twice as often as `values.yaml`. Patches the symptom, not the surface. |
| Allowlist `*.yaml` by basename | Far too broad — would exempt Kubernetes manifests, kuttl tests and workflow-adjacent YAML on any bot branch. |
| Drop condition (b) for bot authors | Removes the asymmetry ADR-1152 exists to create: a Renovate PR that also edits `core/` would skip the doc gates entirely. |
| Have Renovate write a deliverables checklist | Not expressible in Renovate's PR-body templating, and a bot-authored checklist would be a rubber stamp rather than a gate. |

## Verification

`scripts/ci/test-classify-dependency-pr.sh` grew from 13 to 19 cases. The two
that matter are the negatives: `deploy/helm/vmafx/values.yaml` **+**
`core/src/model.c` must be NOT exempt (exit 1), and a human-authored change to
`deploy/helm/vmafx/values.yaml` must be NOT exempt (exit 1). Both pass, so the
widened allowlist provably did not weaken the gate.
