<!-- markdownlint-disable MD013 -->
# AGENTS.md — pkg/scoringscope

Input-URI allowlist validation and path resolution (ADR-1577). Parent: [../../AGENTS.md](../../AGENTS.md).

## Rebase-sensitive invariants

1. Deny by default: empty configured root list refuses every input URI; `..` segments and relative local paths always fail validation across local filesystem, HTTP(S), and rclone schemes.
2. Symlink vs lexical split: `Resolve` evaluates symlinks (`filepath.EvalSymlinks`) on local roots and paths so synchronous scoring reads resolved real path; `Check` validates lexically without filesystem access for queued job submission.
3. Tenant-root wiring, `{tenant}` substitution restrictions, and `PullWork` `Job.scoring_roots` propagation: read [scoring-roots](../../cmd/vmafx-controller/AGENTS.d/scoring-roots.md).
