---
paths:
  - scripts/ci/classify-dependency-pr.sh
  - scripts/ci/test-classify-dependency-pr.sh
  - scripts/ci/tests/test_renovate_file_patterns.py
  - renovate.json
invariant: Bot exemption = bot author AND every changed path allowlisted; new pinning surface -> allowlist entry plus fixture.
---
<!-- markdownlint-disable MD013 MD060 -->
# Adding a Renovate-managed surface (ADR-1152)

`tests/test_renovate_file_patterns.py` validates positive file-selection
fixtures for custom managers. `managerFilePatterns` regexes have one slash
delimiter at each end; doubled delimiters silently select no files despite
passing Renovate's schema validator. Keep base-image custom manager's
config-plus-mirror set paired with built-in Docker manager exclusions.
`test-renovate-file-patterns` pre-commit hook runs these fixtures whenever
Renovate configuration or test changes.

`classify-dependency-pr.sh` exempts bot PR only when **every** changed path
matches its allowlist — one unmatched path fails whole PR; bot cannot
write deliverables checklist to recover. New dependency-pinning
surface appearing in tree (new chart under `deploy/helm/`, new compose
file, new container build file outside `docker/`) -> add it to
`is_allowed_dependency_path` **and** add fixture case to
`test-classify-dependency-pr.sh` in same change.

`build-config.env` allowance = exact root-path match (ADR-1231).
Never replace with env-file glob or basename match: nested build
configs and unrelated runtime env files must still fail path condition.

Root `Makefile` allowance = exact root-path match too: Renovate
`custom.regex` bumps `RUFF_VERSION` / `BLACK_VERSION` there with
pre-commit revs (#1588). Nested Makefiles, `*.mk` stay gated.

Two invariants test suite pins deliberately — do not "simplify" them away:

- Widening allowlist must never drop conjunction with condition (a).
  Human-authored PR touching allowlisted path must still be gated.
- Bot PR touching allowlisted path **and** source code must still be
  gated. That asymmetry = entire point of gate.

Derive additions from what Renovate edits (`gh pr list --author
app/renovate` and diff file lists), not from what looks like manifest —
see [`docs/research/1152-dependency-classifier-surface-audit.md`](../../../docs/research/1152-dependency-classifier-surface-audit.md).

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `classify-dependency-pr.sh` | `rule-enforcement.yml` — `deep-dive-checklist` and `doc-substance-check` jobs ([ADR-1152](../../../docs/adr/1152-dependency-pr-gate-exemption.md)) | Reads `$PR_AUTHOR`, `$HEAD_REF`, `$BASE_SHA`, `$HEAD_SHA` from workflow env. exemption is author-AND-path-gated and must never be widened to path glob alone. Bot identity requires `renovate[bot]` / `dependabot[bot]` (or `app/renovate` / `app/dependabot`), or `renovate/*` / `dependabot/*` branch, AND all changed paths must be in explicit manifest/lockfile allowlist. Only `requirements/*` has subtree authority for generic `.in` and `manifest.json` files; never restore basename-wide `*.in` or `manifest.json` exemptions (`core/include/libvmaf/version.h.in` is negative fixture). Bot PRs touching source code must still satisfy both documentation gates. Test suite: `scripts/ci/test-classify-dependency-pr.sh`. |
| `test-classify-dependency-pr.sh` | (local-only fixture driver, not invoked by CI) | Run before pushing changes to `classify-dependency-pr.sh`; exercises dependency-only and mixed source diffs, named `requirements*.in` inputs versus unrelated `.in` templates, non-bot authors, and real PR fixtures (#1206, #1207, #1212, #1214). |
