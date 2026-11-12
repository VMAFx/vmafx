---
paths:
  - cmd/vmafx-controller/scoring_scope*.go
  - cmd/vmafx-controller/auth/tenants_scoring_test.go
  - pkg/scoringscope/*.go
invariant: Deny input paths without configured roots; symlink-resolved on score, lexical on SubmitJob.
---
<!-- markdownlint-disable MD013 MD029 MD060 -->
# Tenant scoring roots (ADR-1577)

## Invariants

### scoring roots (ADR-1577)

1. **Deny by default** (`scoring_scope.go`, `pkg/scoringscope`): no roots =
   every input refused (`PermissionDenied`, 403). Registry mode: roots from
   `TenantSpec.Scoring` (`TenantRegistry.ScoringRoots`); else
   `VMAFX_SCORING_ROOTS` with `{tenant}`; both = startup error. Tenant ID with
   `/`, `\`, `:`, `.`, `..` never substituted.
2. **Where checked**: `Score` / `POST /v1/score` resolve (symlinks) and score
   the real path; `SubmitJob` lexical; `PullWork` sets `Job.scoring_roots`
   (only there, never GetJob/StreamJobs). Guards: `scoring_scope_test.go`,
   `auth/tenants_scoring_test.go`, `pkg/scoringscope` tests.
