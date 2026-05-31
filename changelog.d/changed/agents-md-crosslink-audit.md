## chore(docs): AGENTS.md crosslink + ADR-citation audit (15 files)

Added "Parent:" backlinks to 14 nested `AGENTS.md` files that previously
lacked one, and fixed one stale ADR citation:

- Parent backlink added: `dev/`, `.github/`, `docs/research/`,
  `ai/src/aiutils/`, `scripts/ci/`, `scripts/lib/`,
  `tools/vmaf-tune/`, `tools/vmaf-roi-score/`, `tools/external-bench/`,
  `cmd/vmafx-tune/`, `cmd/vmafx-operator/`, `bindings/rust/vmafx-sys/`,
  `core/src/mcp/3rdparty/cJSON/`, `core/src/feature/metal/`,
  `core/src/feature/hip/`.
- Stale ADR citation fixed: `tools/external-bench/AGENTS.md` referenced
  ADR-0332 (worktree drift guard, unrelated); corrected to ADR-0368
  (external-bench wrapper-only architecture) + ADR-0656 (wrapper schema).
- Stale `PR #[TBD] / ADR-[TBD]` placeholder in
  `core/src/feature/hip/AGENTS.md` clarified — no follow-up ADR was
  filed for the 2026-05-16 GPU audit; line numbers updated from the
  obsolete 316/322 to the current 212/359/364 post-refactor positions.

PR #328 (AGENTS.md sweep titles + trees) skipped to avoid scope overlap.

No code changes; no user-visible behaviour change.
