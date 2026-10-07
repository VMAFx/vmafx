-- SPDX-License-Identifier: EUPL-1.2
-- Copyright 2026 Lusoris
--
-- PostgreSQL store of the controller (ADR-2350 D1-D3): jobs, their attempts
-- and node sessions. Node work is claimed with leases: the attempt number is
-- the fencing token, a lease expires unless the node's heartbeat extends it.
-- Row-level security keeps every tenant query inside its tenant even when a
-- WHERE clause is missing; maintenance runs with vmafx.maintenance = on.

CREATE TABLE node_sessions (
    id            uuid        PRIMARY KEY,
    tenant_id     text        NOT NULL CHECK (tenant_id <> ''),
    node_id       text        NOT NULL CHECK (node_id <> ''),
    token_sha256  bytea       NOT NULL CHECK (octet_length(token_sha256) = 32),
    capability    jsonb       NOT NULL DEFAULT '{}'::jsonb,
    created_at    timestamptz NOT NULL DEFAULT now(),
    last_seen_at  timestamptz NOT NULL DEFAULT now(),
    expires_at    timestamptz NOT NULL
);
CREATE INDEX node_sessions_expires_idx ON node_sessions (expires_at);

CREATE TABLE jobs (
    id                 uuid             PRIMARY KEY,
    tenant_id          text             NOT NULL CHECK (tenant_id <> ''),
    idempotency_key    text             CHECK (idempotency_key <> ''),
    status             text             NOT NULL DEFAULT 'pending'
        CHECK (status IN ('pending', 'running', 'completed', 'failed', 'cancelled')),
    spec               jsonb            NOT NULL,
    backend            text             NOT NULL DEFAULT '',
    priority           integer          NOT NULL DEFAULT 0,
    -- Claims so far; the fencing token of the running attempt.
    attempt            integer          NOT NULL DEFAULT 0 CHECK (attempt >= 0),
    -- Attempts that ended because their lease expired (a released attempt
    -- does not count); max_lost_attempts of them fail the job.
    lost_attempts      integer          NOT NULL DEFAULT 0 CHECK (lost_attempts >= 0),
    max_lost_attempts  integer          NOT NULL DEFAULT 3 CHECK (max_lost_attempts >= 1),
    available_at       timestamptz      NOT NULL DEFAULT now(),
    lease_session      uuid             REFERENCES node_sessions (id) ON DELETE SET NULL,
    lease_node         text,
    lease_expires_at   timestamptz,
    score              double precision,
    features           jsonb,
    error              text,
    created_at         timestamptz      NOT NULL DEFAULT now(),
    updated_at         timestamptz      NOT NULL DEFAULT now(),
    finished_at        timestamptz,
    CONSTRAINT jobs_running_has_lease CHECK ((status = 'running') = (lease_expires_at IS NOT NULL)),
    CONSTRAINT jobs_finished_is_terminal
        CHECK ((status IN ('completed', 'failed', 'cancelled')) = (finished_at IS NOT NULL))
);
CREATE UNIQUE INDEX jobs_idempotency_idx ON jobs (tenant_id, idempotency_key)
    WHERE idempotency_key IS NOT NULL;
CREATE INDEX jobs_claim_idx ON jobs (tenant_id, priority DESC, created_at, id)
    WHERE status = 'pending';
CREATE INDEX jobs_lease_idx ON jobs (lease_expires_at) WHERE status = 'running';
CREATE INDEX jobs_tenant_status_idx ON jobs (tenant_id, status, created_at);

CREATE TABLE job_attempts (
    job_id      uuid        NOT NULL REFERENCES jobs (id) ON DELETE CASCADE,
    attempt     integer     NOT NULL CHECK (attempt >= 1),
    tenant_id   text        NOT NULL,
    session_id  uuid        NOT NULL,
    node_id     text        NOT NULL,
    started_at  timestamptz NOT NULL DEFAULT now(),
    ended_at    timestamptz,
    outcome     text CHECK (outcome IN ('completed', 'failed', 'released', 'expired', 'cancelled')),
    error       text,
    PRIMARY KEY (job_id, attempt),
    CONSTRAINT job_attempts_ended_has_outcome CHECK ((ended_at IS NULL) = (outcome IS NULL))
);

ALTER TABLE node_sessions ENABLE ROW LEVEL SECURITY;
ALTER TABLE node_sessions FORCE ROW LEVEL SECURITY;
ALTER TABLE jobs ENABLE ROW LEVEL SECURITY;
ALTER TABLE jobs FORCE ROW LEVEL SECURITY;
ALTER TABLE job_attempts ENABLE ROW LEVEL SECURITY;
ALTER TABLE job_attempts FORCE ROW LEVEL SECURITY;

CREATE POLICY node_sessions_tenant ON node_sessions
    USING (tenant_id = current_setting('vmafx.tenant_id', true)
           OR current_setting('vmafx.maintenance', true) = 'on')
    WITH CHECK (tenant_id = current_setting('vmafx.tenant_id', true)
                OR current_setting('vmafx.maintenance', true) = 'on');
CREATE POLICY jobs_tenant ON jobs
    USING (tenant_id = current_setting('vmafx.tenant_id', true)
           OR current_setting('vmafx.maintenance', true) = 'on')
    WITH CHECK (tenant_id = current_setting('vmafx.tenant_id', true)
                OR current_setting('vmafx.maintenance', true) = 'on');
CREATE POLICY job_attempts_tenant ON job_attempts
    USING (tenant_id = current_setting('vmafx.tenant_id', true)
           OR current_setting('vmafx.maintenance', true) = 'on')
    WITH CHECK (tenant_id = current_setting('vmafx.tenant_id', true)
                OR current_setting('vmafx.maintenance', true) = 'on');
