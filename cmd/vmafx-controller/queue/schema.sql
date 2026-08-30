-- SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-- Copyright 2026 Lusoris
--
-- cmd/vmafx-controller/queue/schema.sql — SQLite schema for job persistence.
--
-- Used by queue.Queue to persist jobs across controller restarts.
-- Applied once at startup via queue.New(); idempotent (CREATE TABLE IF NOT EXISTS).
--
-- ADR-0711: vmafx-controller Phase 4b.1 scope expansion.

-- jobs holds every job ever submitted to the controller.
CREATE TABLE IF NOT EXISTS jobs (
    -- UUID v4 assigned by the controller at submission time.
    id          TEXT NOT NULL PRIMARY KEY,

    -- Lifecycle state: pending | running | completed | failed | cancelled.
    status      TEXT NOT NULL DEFAULT 'pending',

    -- Scoring parameters serialised as JSON:
    --   {"reference":"…","distorted":"…","model":"…","backend":"…"}
    scoring     TEXT NOT NULL DEFAULT '{}',

    -- The node_id currently assigned to run this job, or NULL.
    assigned_node TEXT,

    -- Aggregate VMAF score, populated when status = 'completed'.
    score       REAL,

    -- Per-feature scores serialised as JSON, populated when status = 'completed'.
    features    TEXT,

    -- Latest error message, populated when status = 'failed'.
    error       TEXT,

    -- Tenant identifier extracted from the submitter's JWT "tid" claim.
    -- All reads are scoped to this value (ADR-0794).
    tenant_id   TEXT NOT NULL DEFAULT '',

    -- Unix epoch seconds.
    created_at  INTEGER NOT NULL,
    updated_at  INTEGER NOT NULL
);

-- Index for the scheduler's hot path: fetch the oldest PENDING job that
-- matches a given backend capability.
CREATE INDEX IF NOT EXISTS idx_jobs_status_created
    ON jobs (status, created_at);

-- Index for tenant-scoped queries (ADR-0794).
CREATE INDEX IF NOT EXISTS idx_jobs_tenant_status
    ON jobs (tenant_id, status, created_at);
