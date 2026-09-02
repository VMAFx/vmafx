### Docs: ADR-0108 compliance audit + AGENTS.md D3 gap fixes (2026-05-29)

Five PRs merged on 2026-05-28 audited for ADR-0108 six-deliverable compliance.
Score: 28/30 (93 %). Two D3 (AGENTS.md invariant note) gaps identified and
remediated:

- Root `AGENTS.md` §13: added `libvmaf/ → core/` rename invariant row (PR #1571,
  ADR-0700) and `vmafx-server [http]` dep-group invariant row (PR #1583, ADR-0701).
- `mcp-server/AGENTS.md`: added HTTP transport dispatch-order invariant note.
- Fixed two stale `libvmaf/AGENTS.md` path references → `core/AGENTS.md` in root §13.
- Research digest at `docs/research/adr-0108-compliance-audit-2026-05-29.md`.

ADR-0810.
