---
paths:
  - cmd/vmafx-controller/store/**/*.go
  - cmd/vmafx-controller/store/migrations/postgres/*.sql
  - cmd/vmafx-controller/store/queries/postgres/*.sql
  - cmd/vmafx-controller/internal/storetest/storetest.go
invariant: Attempt + session fencing; WHERE + FORCE RLS tenant isolation; sqlc generated; digested pg image.
---
<!-- markdownlint-disable MD013 MD029 MD060 -->
# PostgreSQL store invariants (`store/`, ADR-2350)

## Invariants

### store package (ADR-2350, WP17; not wired yet)

PostgreSQL store replacing SQLite queue. `store/`: migrations
(`migrations/postgres/`, embedded, golusoris `db/migrate`), sqlc queries
(`queries/postgres/` -> generated `pgdb/`), Go API on top.

1. **Leased claims, attempt = fencing token** (Q-122): `Claim` = `FOR UPDATE
   SKIP LOCKED`, opens attempt n + `job_attempts` row. `Report` / `Release`
   / `ExtendLeases` UPDATE compares `attempt` AND `lease_session` AND
   `status = 'running'`. Never drop either compare: same session reclaiming
   after expiry is fenced only by `attempt`
   (`TestFencingRefusesAnOldAttemptOfTheSameSession`). Repeat of own ended
   attempt = no-op success only for outcomes in `reportRepeatable` /
   `releaseRepeatable`; report of released/expired attempt = `ErrFenced`.
2. **Tenant twice**: tenant in every WHERE + RLS (`FORCE ROW LEVEL
   SECURITY`, policies on `vmafx.tenant_id` / `vmafx.maintenance`). Tenant
   ops via `inTenant`, cross-tenant only via `inMaintenance` (expiry,
   counts). App role must not be superuser (superuser bypasses RLS); tests
   run as owner role `vmafx_app` (`TestRowLevelSecurityHidesOtherTenants`).
3. **Generated, never hand-edited**: `pgdb/*.go` = sqlc output + SPDX header.
   Edit SQL, then `python3 scripts/codegen/sqlc_generate.py --write` (sqlc
   pinned in `build-config.env`: `SQLC_VERSION` + per-platform SHA-256).
   Gate: Meson `test_sqlc_generated_current` (77 + reason without pinned
   binary). Schema change = new migration file + raise `SchemaVersion`.
4. **Bounded loops** (HISS-02): `MaxHeartbeatJobs` 64, `MaxBackends` 16,
   `MaxListJobs` 10000, `MaxExpiryBatch` 1000; larger input -> `ErrInvalid`.
5. **Tests need Docker** (golusoris `testutil/pg`; image `storetest.Image`,
   migrations also on `storetest.OldestImage`). No skip-on-missing-Docker
   added here; `-short` skips (testutil). `storetest.Image` / `OldestImage`
   stay `tag@sha256:<index digest>`: testutil/pg >= golusoris v0.13.0 fails
   test on undigested image. Tag bump = new digest, same edit. Renovate
   custom manager (`depNameTemplate: postgres`) owns both constants;
   `OldestImage` held on 16.x by package rule.
