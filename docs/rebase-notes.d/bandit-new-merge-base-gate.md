## Merge-base Bandit gate and outgoing scanner snapshots (2026-11-11)

ADR-3530. No rebase impact: the scanner scripts, hook configuration, dependency locks and agent notes are fork-local tooling; no Netflix source, public ABI or golden assertion changes. Preserve the shared revision selection in `scripts/lib/new_scanner_findings.py`, the Bandit configuration in `bandit.yml`, and the paired tests when syncing tooling. Regenerate `scripts/ci/AGENTS-gates.md` from `scripts/ci/AGENTS.d/bandit-new.md`; ADR indexes remain render-only outputs.
