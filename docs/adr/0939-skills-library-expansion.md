<!-- markdownlint-disable MD060 -->
# ADR-0939: Skills library expansion — MCP, k8s, audit, bisect consolidation

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris (req)
- **Tags**: agents, skills, mcp, k8s, audit, fork-local

## Context

The `.claude/skills/` library carries 25 skills covering build, lint, ports,
backends, SIMD, models, and AI training. Three flagship surfaces grew on the
fork during the VMAFX rebrand (Phase 4 / Phase 4b) without a matching
scaffolding skill:

1. The MCP tool surface now spans **two parallel implementations** — the
   original Python server at `mcp-server/vmaf-mcp/` plus the new Go server at
   `cmd/vmafx-mcp/` (ADR-0703). Adding a new tool requires editing both, plus
   a per-tool doc page (ADR-0100 per-surface bar) and a parity check. Doing
   this by hand drifts the two servers (already happened twice during the
   merge train) and silently breaks IDE-side schema compatibility.
2. The Kubernetes operator at `cmd/vmafx-operator/` (ADR-0714) ships three
   CRDs today (`VmafxJob`, `VmafxNode`, `VmafxModelTraining`). Adding a fourth
   touches eight files in lock-step (types, controller, controller test, CRD
   YAML, RBAC, values.yaml, operator.md, AGENTS.md) — a high-overhead step
   that disincentivises evolving the operator surface.
3. `scripts/dev/project_modernization_audit.py` is the canonical
   "what's-left" scanner but has no slash-skill wrapper, so it gets
   re-invoked with ad-hoc paths and outputs land in unpredictable locations
   instead of the de-facto `/tmp/modernization-audit-YYYY-MM-DD.md`
   convention used by every recent planning session.

Separately, `/bisect-regression` and `/bisect-model-quality` both grew
SKILL.md-only definitions without driver scripts. When each gets one, both
will need the same boilerplate: clean-tree gate, auto-stash push/pop,
`git bisect reset` on exit, markdown verdict rendering, exit-code constants
matching `git bisect run` semantics. Letting each skill copy-paste that
boilerplate would diverge subtly (proven precedent: every earlier wrapper
diverged on stash-restore behaviour during the merge train).

## Decision

Add three new flagship scaffolding skills and one shared shell library:

1. `.claude/skills/add-mcp-tool/` — scaffolds a parallel Go + Python MCP tool
   handler with registration patches, smoke tests, per-tool doc page, and
   changelog fragment. Enforces parity at scaffold time (refuses to ship a
   tool to only one server) and bakes in the `isError=True` reminder from
   the `project_mcp_iserror_must_be_true` memory entry.
2. `.claude/skills/add-k8s-resource/` — scaffolds a new CRD + kubebuilder
   controller + RBAC + helm chart entry following the
   `VmafxJob`/`VmafxNode`/`VmafxModelTraining` precedent. Controller ships
   disabled by default (ADR-0714 Stage 1 posture); RBAC defaults to a tight
   verb set (no `delete`, no `*`).
3. `.claude/skills/audit-modernization/` — thin wrapper around
   `scripts/dev/project_modernization_audit.py` with the de-facto
   `/tmp/modernization-audit-YYYY-MM-DD.md` output convention,
   `--include-archives` toggle, and a guardrail against auto-dispatching
   agents on findings.
4. `.claude/skills/lib/bisect-common.sh` — shared shell library sourced by
   the two bisect skills (`bisect-regression`, `bisect-model-quality`). Owns
   the clean-tree gate, auto-stash push/pop, exit-code constants, and
   markdown verdict rendering. Both skills also gain a `scaffold.sh` driver
   that sources the library.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Single mega-skill `/add-*` with subcommands | One entry point | Hides the per-surface checklists; harder to discover; breaks the existing `/add-gpu-backend`, `/add-feature-extractor`, `/add-model`, `/add-simd-path` precedent | Inconsistent with the established skill-per-surface pattern |
| Skip the MCP scaffold; rely on copy-paste | Zero new code | The Go ↔ Python parity drift problem persists; per-tool doc page still skipped half the time | Already proven to drift during the merge train |
| Skip the k8s scaffold; tell operators to hand-edit | Zero new code | Eight files in lock-step is exactly the kind of error-prone surface a scaffold should cover | Operator-evolution friction is the gating bug |
| Inline `audit-modernization` as a bash one-liner in CLAUDE.md | No new file | Loses discoverability; CLAUDE.md not the right home for runnable shortcuts | Skills are the discoverability surface |
| Keep duplicated bisect logic; document the divergence | Less file movement | Divergence already proven; future skills sourcing the same boilerplate (e.g. a future `/bisect-perf-snapshot`) inherit the same trap | Shared lib is the lower-cost option |
| Helper as Python module instead of bash | Richer error handling | Bash matches the existing skill pattern; sourcing a Python module from bash is a worse interface | Match the existing precedent |

## Consequences

- **Positive**:
  - Adding an MCP tool / CRD / running an audit becomes a one-line skill
    invocation with the per-surface doc bar (ADR-0100) baked in.
  - Bisect skills now have driver scripts and share boilerplate; future
    divergence is impossible without editing the shared lib.
  - Scaffolds refuse to clobber existing files, refuse to ship one-server
    parity gaps, and refuse to grant `delete`/`*` RBAC verbs without an
    ADR — guardrails are codified rather than tribal.
- **Negative**:
  - Four new files in `.claude/skills/` + one shared lib. Marginal
    maintenance cost; offset by the boilerplate the scaffolds remove.
  - Skills depend on the surfaces they scaffold staying in their current
    paths (`cmd/vmafx-mcp/`, `cmd/vmafx-operator/`,
    `scripts/dev/project_modernization_audit.py`). If those move, the
    skills must move in lock-step — surfaced in `docs/rebase-notes.md`.
- **Neutral / follow-ups**:
  - Future evolution: the `add-mcp-tool` scaffold should learn to emit the
    `tools.go` registration block automatically once the parser lands
    (Phase 5 backlog).
  - Future evolution: `add-k8s-resource` should learn to invoke
    `controller-gen` directly once the helm chart's CRD generation pipeline
    is wired (currently a manual `make manifests` step).

## References

- [ADR-0100](0100-project-wide-doc-substance-rule.md) — per-surface doc bar
- [ADR-0703](0703-go-grpc-scoring-service.md) — Go MCP server (parity contract)
- [ADR-0709](0709-vmafx-phase-4b-distributed-platform.md) — Phase 4b parent
- [ADR-0714](0714-vmafx-operator-kubebuilder-skeleton.md) — operator skeleton
- `req` — user direction (paraphrased): add the three missing flagship skills
  (MCP tool, k8s resource, modernization audit) and consolidate the two
  bisect skills onto a shared `lib/bisect-common.sh` so the
  clean-tree / stash / verdict-rendering boilerplate lives in one place.
