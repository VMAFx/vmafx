-- SPDX-License-Identifier: EUPL-1.2
-- Copyright 2026 Lusoris
--
-- Leased claims of node work (sqlc input, ADR-2350 D2). The attempt number is
-- the fencing token: every write of a running attempt compares it together
-- with the session that holds the lease.

-- name: ClaimNext :one
WITH next AS (
    SELECT j.id FROM jobs j
    WHERE j.tenant_id = @tenant_id
      AND j.status = 'pending'
      AND j.available_at <= now()
      AND (j.backend = '' OR j.backend = ANY(@backends::text[]))
    ORDER BY j.priority DESC, j.created_at, j.id
    LIMIT 1
    FOR UPDATE SKIP LOCKED
)
UPDATE jobs
SET status = 'running', attempt = jobs.attempt + 1, lease_session = @session_id::uuid,
    assigned_node = @node_id::text, lease_expires_at = now() + make_interval(secs => @lease_seconds::float8),
    updated_at = now()
FROM next
WHERE jobs.id = next.id
RETURNING jobs.*;

-- name: InsertAttempt :exec
INSERT INTO job_attempts (job_id, attempt, tenant_id, session_id, node_id)
VALUES (@job_id, @attempt, @tenant_id, @session_id, @node_id);

-- name: EndAttempt :exec
UPDATE job_attempts
SET ended_at = now(), outcome = @outcome::text, error = sqlc.narg('error')
WHERE job_id = @job_id AND attempt = @attempt AND ended_at IS NULL;

-- name: GetAttempt :one
SELECT * FROM job_attempts WHERE job_id = @job_id AND attempt = @attempt AND tenant_id = @tenant_id;

-- name: ExtendLeases :execrows
UPDATE jobs
SET lease_expires_at = now() + make_interval(secs => @lease_seconds::float8), updated_at = now()
WHERE tenant_id = @tenant_id AND lease_session = @session_id::uuid AND status = 'running'
  AND id = ANY(@ids::uuid[]);

-- name: FinishAttempt :execrows
UPDATE jobs
SET status = @status, score = sqlc.narg('score'), features = sqlc.narg('features'),
    error = sqlc.narg('error'), lease_session = NULL, lease_expires_at = NULL,
    finished_at = now(), updated_at = now()
WHERE id = @id AND tenant_id = @tenant_id AND attempt = @attempt AND status = 'running'
  AND lease_session = @session_id::uuid;

-- name: ReleaseAttempt :execrows
UPDATE jobs
SET status = 'pending', lease_session = NULL, assigned_node = NULL, lease_expires_at = NULL,
    available_at = now(), updated_at = now()
WHERE id = @id AND tenant_id = @tenant_id AND attempt = @attempt AND status = 'running'
  AND lease_session = @session_id::uuid;

-- name: ExpiredLeases :many
SELECT id, tenant_id, attempt, lost_attempts, max_lost_attempts FROM jobs
WHERE status = 'running' AND lease_expires_at < now()
ORDER BY lease_expires_at
LIMIT @max_rows
FOR UPDATE SKIP LOCKED;

-- name: RequeueExpired :exec
UPDATE jobs
SET status = 'pending', lost_attempts = lost_attempts + 1,
    available_at = now() + make_interval(secs => @delay_seconds::float8),
    lease_session = NULL, assigned_node = NULL, lease_expires_at = NULL, updated_at = now()
WHERE id = @id AND attempt = @attempt AND status = 'running';

-- name: FailExpired :exec
UPDATE jobs
SET status = 'failed', lost_attempts = lost_attempts + 1, error = @error::text,
    lease_session = NULL, lease_expires_at = NULL,
    finished_at = now(), updated_at = now()
WHERE id = @id AND attempt = @attempt AND status = 'running';
