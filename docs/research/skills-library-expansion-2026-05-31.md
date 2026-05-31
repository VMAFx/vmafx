<!-- markdownlint-disable MD060 -->
# Research digest — Skills library expansion (2026-05-31)

Backs [ADR-0939](../adr/0939-skills-library-expansion.md). Modernization audit
item #20 — add three missing flagship scaffolding skills (MCP tool, k8s CRD,
modernization audit) and consolidate the two bisect skills onto a shared
shell library so the operator-tree boilerplate cannot diverge.

## Scope of the gap

The `.claude/skills/` library carries 25 skills, but three high-traffic
surfaces have no scaffolding counterpart:

| Surface | Current state | Failure mode without a scaffold |
|---|---|---|
| MCP tools (`cmd/vmafx-mcp/` Go + `mcp-server/vmaf-mcp/` Python) | 16 tools live; parity contract in ADR-0703 | Parity drift between Go + Python (proven twice on the merge train); per-tool doc page (ADR-0100 bar) skipped; `isError=True` reminder forgotten (project_mcp_iserror_must_be_true) |
| k8s CRDs (`cmd/vmafx-operator/`) | 3 CRDs live (`VmafxJob`, `VmafxNode`, `VmafxModelTraining`) per ADR-0714 | Eight files in lock-step per new CRD; tight RBAC verb-set rule (no `delete`, no `*`) easy to forget; helm `crds/` and `values.yaml` desync |
| Modernization audit (`scripts/dev/project_modernization_audit.py`) | 703-line read-only scanner | Re-invoked with ad-hoc paths each session; outputs land in unpredictable locations; downstream automation cannot rely on the de-facto `/tmp/modernization-audit-YYYY-MM-DD.md` convention |

The two existing bisect skills (`/bisect-regression`,
`/bisect-model-quality`) had SKILL.md definitions but no driver scripts. When
each gets one, both need identical operator-tree boilerplate. Letting each
copy-paste guarantees subtle divergence (the merge train has burned this
pattern before — every earlier shell-driver wrapper diverged on stash-restore).

## Files touched (proposed)

- `.claude/skills/add-mcp-tool/` (new) — SKILL.md, scaffold.sh, 5 templates
- `.claude/skills/add-k8s-resource/` (new) — SKILL.md, scaffold.sh, 5 templates
- `.claude/skills/audit-modernization/` (new) — SKILL.md, scaffold.sh
- `.claude/skills/lib/bisect-common.sh` (new) — shared helpers
- `.claude/skills/bisect-regression/` (extend) — scaffold.sh + SKILL.md
  cross-reference
- `.claude/skills/bisect-model-quality/` (extend) — scaffold.sh + SKILL.md
  cross-reference
- `docs/adr/0939-skills-library-expansion.md` (new)
- `docs/adr/_index_fragments/0939-skills-library-expansion.md` (new)
- `docs/adr/_index_fragments/_order.txt` (append)
- `changelog.d/added/skills-library-expansion.md` (new)
- `docs/research/skills-library-expansion-2026-05-31.md` (this file)
- `docs/rebase-notes.md` (append)

## Decision matrix

Captured in
[ADR-0939 §Alternatives considered](../adr/0939-skills-library-expansion.md#alternatives-considered).
Headline: the skill-per-surface pattern was already established by
`/add-gpu-backend`, `/add-feature-extractor`, `/add-simd-path`, `/add-model`;
the three new skills follow the same shape rather than introducing a mega
`/add-*` subcommand router.

## Validation performed

- `bash -n` syntax-check on every new shell script (lib + 5 scaffolds) →
  clean.
- Sourced `lib/bisect-common.sh` standalone → constants resolve, `bisect_log`
  prints the expected prefix.
- Ran `bash .claude/skills/audit-modernization/scaffold.sh --max-findings=3
  --out=/tmp/wt-audit-test.md` against the worktree → produced a 697-finding
  report, summary lines emitted, return code 0.
- Templates do not collide with any existing path under
  `cmd/vmafx-mcp/`, `cmd/vmafx-operator/`, `api/vmafx/v1/`,
  `deploy/helm/vmafx/`, `docs/mcp/tools/`, `docs/k8s/crds/`.

## Open follow-ups (NOT part of this PR)

- `add-mcp-tool` could parse `cmd/vmafx-mcp/tools.go` to auto-insert the
  `addRawTool` registration block instead of relying on the operator. Phase 5
  backlog; not blocking.
- `add-k8s-resource` could invoke `controller-gen` directly to regenerate the
  CRD YAML rather than relying on `make manifests`. Blocked on the helm
  chart's CRD-generation pipeline; not in scope here.
- A future `/bisect-perf-snapshot` skill would source the same
  `lib/bisect-common.sh` library; the shared helpers were sized with that
  third consumer in mind.

## Reproducer

```bash
git checkout chore/skills-library-expansion
bash -n .claude/skills/lib/bisect-common.sh
bash -n .claude/skills/add-mcp-tool/scaffold.sh
bash -n .claude/skills/add-k8s-resource/scaffold.sh
bash -n .claude/skills/audit-modernization/scaffold.sh
bash -n .claude/skills/bisect-regression/scaffold.sh
bash -n .claude/skills/bisect-model-quality/scaffold.sh

# End-to-end smoke (read-only):
bash .claude/skills/audit-modernization/scaffold.sh --max-findings=10 \
  --out=/tmp/smoke-audit.md
head -5 /tmp/smoke-audit.md
```
