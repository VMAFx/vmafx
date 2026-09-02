# Research-0658: Project modernization audit

- **Status**: Active
- **Workstream**: ADR-0658
- **Last updated**: 2026-05-20

## Question

How can the fork keep finding real backlog, scaffold, stub, limitation, and
modernization work after many one-off sweeps have already landed, without
turning stale prose or model-artifact blockers into noisy PR churn?

## Sources

- `.workingdir2/OPEN.md` and `.workingdir2/BACKLOG.md` — current local
  gitignored planning state.
- `docs/state.md` — durable project status, deferred rows, and recently closed
  implementation history.
- `docs/ai/signal-mix-audit.md` and [ADR-0650](../adr/0650-signal-mix-audit.md)
  — precedent for advisory, table-side audit tooling.
- `scripts/AGENTS.md` and `scripts/lib/AGENTS.md` — read-only local state and
  stdlib-only helper expectations.

## Findings

- Pure `rg` searches still work, but they mix very different things:
  actionable `NotImplementedError` / `ENOSYS` code, stale docs, accepted ADR
  history, smoke-only fixture rows, and real blockers such as external model
  weights or dataset access.
- The current project state is split between committed docs and ignored local
  files. Any audit that ignores `.workingdir2` loses the merge-train context;
  any audit that rewrites `.workingdir2` risks deleting editorial judgement.
- AI modernization is not only individual TODO markers. Large one-off families
  such as `train_*`, `export_*`, `eval_*`, and `extract_*` scripts are useful
  cluster signals for future manifest/config consolidation.
- Model registry smoke rows are a separate class of debt. They should be
  visible in reports, but placeholder fixtures are often intentionally blocked
  until a real model or redistribution decision exists.

## Alternatives explored

Ad hoc search won on speed but lost on continuity. It is useful while working
inside one file, but it does not preserve prioritisation across sessions.

CI gating was rejected because the text markers intentionally appear in docs,
historical ADRs, disabled-build contracts, and model cards. A failing required
gate would incentivise wording games instead of useful code work.

Automatic backlog rewriting was rejected because `.workingdir2` is the
human-edited state of record. The audit can produce JSON and Markdown, but
the operator decides what belongs in `OPEN.md` or `BACKLOG.md`.

## Open questions

- Whether future reports should learn explicit suppressions for intentional
  contracts such as disabled optional-backend `-ENOSYS`.
- Whether the script-family cluster output should grow into a dedicated AI
  manifest/schema consolidation PR.
- Whether the model-registry smoke rows should be split into "permanent smoke
  fixture" and "promotion debt" fields in the registry schema.

## Related

- ADRs: [ADR-0658](../adr/0658-project-modernization-audit.md),
  [ADR-0650](../adr/0650-signal-mix-audit.md)
