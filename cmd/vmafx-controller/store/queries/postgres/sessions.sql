-- SPDX-License-Identifier: EUPL-1.2
-- Copyright 2026 Lusoris
--
-- Node sessions (sqlc input, ADR-2350 D2). The token is stored as its SHA-256;
-- a session belongs to one tenant (ADR-1522).

-- name: InsertSession :exec
INSERT INTO node_sessions (id, tenant_id, node_id, token_sha256, capability, expires_at)
VALUES (@id, @tenant_id, @node_id, @token_sha256, @capability,
        now() + make_interval(secs => @session_seconds::float8));

-- name: LiveSession :one
SELECT * FROM node_sessions
WHERE id = @id AND tenant_id = @tenant_id AND token_sha256 = @token_sha256 AND expires_at > now()
FOR SHARE;

-- name: TouchSession :execrows
UPDATE node_sessions
SET last_seen_at = now(), expires_at = now() + make_interval(secs => @session_seconds::float8)
WHERE id = @id AND tenant_id = @tenant_id AND token_sha256 = @token_sha256 AND expires_at > now();

-- name: DeleteExpiredSessions :execrows
DELETE FROM node_sessions WHERE expires_at < now();
