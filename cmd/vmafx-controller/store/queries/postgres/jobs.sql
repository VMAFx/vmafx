-- SPDX-License-Identifier: EUPL-1.2
-- Copyright 2026 Lusoris
--
-- Job queries of the PostgreSQL store (sqlc input, ADR-2350 D2). Every tenant
-- query names the tenant in its WHERE clause; row-level security is the
-- second wall, not the first.

-- name: SetTenant :exec
SELECT set_config('vmafx.tenant_id', @tenant_id::text, true);

-- name: SetMaintenance :exec
SELECT set_config('vmafx.maintenance', 'on', true);

-- name: InsertJob :one
INSERT INTO jobs (id, tenant_id, idempotency_key, spec, backend, priority, max_lost_attempts)
VALUES (@id, @tenant_id, sqlc.narg('idempotency_key'), @spec, @backend, @priority, @max_lost_attempts)
ON CONFLICT (tenant_id, idempotency_key) WHERE idempotency_key IS NOT NULL DO NOTHING
RETURNING *;

-- name: GetJobByIdempotencyKey :one
SELECT * FROM jobs WHERE tenant_id = @tenant_id AND idempotency_key = @idempotency_key;

-- name: NotifyJobs :exec
SELECT pg_notify('vmafx_jobs', @backend::text);

-- name: GetJob :one
SELECT * FROM jobs WHERE id = @id AND tenant_id = @tenant_id;

-- name: ListJobs :many
SELECT * FROM jobs
WHERE tenant_id = @tenant_id
  AND (cardinality(@statuses::text[]) = 0 OR status = ANY(@statuses::text[]))
ORDER BY created_at, id
LIMIT @max_rows;

-- name: CancelJob :one
WITH prev AS (
    SELECT j.id, j.status FROM jobs j
    WHERE j.id = @id AND j.tenant_id = @tenant_id AND j.status IN ('pending', 'running')
    FOR UPDATE
)
UPDATE jobs
SET status = 'cancelled', lease_session = NULL, lease_expires_at = NULL,
    finished_at = now(), updated_at = now()
FROM prev
WHERE jobs.id = prev.id
RETURNING jobs.attempt, prev.status AS previous_status;

-- name: CancelledAmong :many
SELECT id FROM jobs
WHERE tenant_id = @tenant_id AND status = 'cancelled' AND id = ANY(@ids::uuid[]);

-- name: CountActive :many
SELECT status, backend, count(*)::bigint AS jobs
FROM jobs
WHERE status IN ('pending', 'running')
GROUP BY status, backend
ORDER BY status, backend;

-- name: TenantStats :many
SELECT tenant_id, status, count(*)::bigint AS jobs, min(created_at)::timestamptz AS oldest
FROM jobs
WHERE status IN ('pending', 'running')
GROUP BY tenant_id, status
ORDER BY tenant_id, status;

-- name: ImportJob :execrows
INSERT INTO jobs (id, tenant_id, status, spec, backend, assigned_node, score, features, error,
                  created_at, updated_at, finished_at)
VALUES (@id, @tenant_id, @status, @spec, @backend, sqlc.narg('assigned_node'), sqlc.narg('score'),
        sqlc.narg('features'), sqlc.narg('error'), @created_at, @updated_at, sqlc.narg('finished_at'))
ON CONFLICT (id) DO NOTHING;
