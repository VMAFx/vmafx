// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store/store.go — the controller's PostgreSQL store.

// Package store keeps the controller's state in PostgreSQL (ADR-2350 D1-D3):
// jobs, the attempts nodes make at them, and node sessions. No controller
// replica keeps state of its own, so any replica can serve any node and any
// replica can die.
//
// Node work is claimed with leases (ADR-2350 D2, ledger Q-122): a claim opens
// attempt n of a job, n is the fencing token, and every later write of that
// attempt (heartbeat, report, release) compares it together with the session
// that holds the lease. A lease that is not extended expires; the expiry sweep
// returns the job to pending with a delay, or fails it after
// max_lost_attempts expiries. A job therefore gets one terminal result however
// often it was scored.
//
// Every tenant operation runs in a transaction that sets vmafx.tenant_id, and
// row-level security refuses other tenants' rows even where a query forgot
// its tenant condition. Maintenance (expiry, counts) sets vmafx.maintenance.
//
// The queries are sqlc-generated (pgdb/, from queries/postgres/ and
// migrations/postgres/); scripts/codegen/sqlc_generate.py regenerates and
// checks them. The SQLite engine of the standalone profile joins later behind
// the same methods.
package store

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	dbsqlc "github.com/golusoris/golusoris/db/sqlc"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store/pgdb"
)

// Status is the lifecycle state of a job.
type Status string

// The job states; the strings are the values of jobs.status.
const (
	StatusPending   Status = "pending"
	StatusRunning   Status = "running"
	StatusCompleted Status = "completed"
	StatusFailed    Status = "failed"
	StatusCancelled Status = "cancelled"
)

// Errors returned (wrapped) by the store.
var (
	// ErrInvalid is a request the store refuses before reaching the database.
	ErrInvalid = errors.New("store: invalid argument")
	// ErrNotFound is a job that does not exist or belongs to another tenant.
	ErrNotFound = errors.New("store: job not found")
	// ErrSessionInvalid is a node session that is unknown, expired, of
	// another tenant, or presented with the wrong token.
	ErrSessionInvalid = errors.New("store: node session unknown, expired or not the caller's")
	// ErrFenced is a write of an attempt that is not the running attempt of
	// the caller's session (a newer attempt took over, or the job was never
	// leased to it). Nothing was written.
	ErrFenced = errors.New("store: not the running attempt of this session")
	// ErrIdempotencyConflict is a submission whose idempotency key names an
	// existing job with a different specification, backend or priority.
	ErrIdempotencyConflict = errors.New("store: idempotency key reused with a different job")
)

// Bounds of the store's loops and result sets (HISS-02).
const (
	// MaxHeartbeatJobs is the most running jobs one heartbeat may name; it
	// equals the node's slot limit (controller maxHeartbeatJobs).
	MaxHeartbeatJobs = 64
	// MaxListJobs caps one List result.
	MaxListJobs = 10000
	// MaxExpiryBatch caps the leases one ExpireLeases call handles.
	MaxExpiryBatch = 1000
	// MaxBackends caps the backends one claim may name.
	MaxBackends = 16
)

// Job is a snapshot of one job.
type Job struct {
	ID              uuid.UUID
	TenantID        string
	IdempotencyKey  string
	Status          Status
	Spec            json.RawMessage
	Backend         string
	Priority        int32
	Attempt         int32
	LostAttempts    int32
	MaxLostAttempts int32
	AvailableAt     time.Time
	// AssignedNode is the node of the current or last attempt.
	AssignedNode   string
	LeaseExpiresAt *time.Time
	Score          *float64
	Features       map[string]float64
	Error          string
	CreatedAt      time.Time
	UpdatedAt      time.Time
	FinishedAt     *time.Time
}

// Postgres is the store on a PostgreSQL pool. It is safe for concurrent use.
type Postgres struct {
	pool *pgxpool.Pool
}

// NewPostgres returns the store on pool. The schema must be migrated
// (MigratePostgres); CheckSchema reports whether it is.
func NewPostgres(pool *pgxpool.Pool) *Postgres {
	return &Postgres{pool: pool}
}

// inTenant runs fn in a transaction scoped to tenantID by row-level security.
func (s *Postgres) inTenant(ctx context.Context, tenantID string, fn func(context.Context, *pgdb.Queries) error) error {
	if tenantID == "" {
		return fmt.Errorf("%w: empty tenant", ErrInvalid)
	}
	return dbsqlc.WithTx(ctx, s.pool, func(ctx context.Context, tx pgx.Tx) error {
		q := pgdb.New(tx)
		if err := q.SetTenant(ctx, tenantID); err != nil {
			return fmt.Errorf("store: scope transaction to tenant: %w", err)
		}
		return fn(ctx, q)
	})
}

// inMaintenance runs fn in a transaction that row-level security lets see
// every tenant.
func (s *Postgres) inMaintenance(ctx context.Context, fn func(context.Context, *pgdb.Queries) error) error {
	return dbsqlc.WithTx(ctx, s.pool, func(ctx context.Context, tx pgx.Tx) error {
		q := pgdb.New(tx)
		if err := q.SetMaintenance(ctx); err != nil {
			return fmt.Errorf("store: open maintenance transaction: %w", err)
		}
		return fn(ctx, q)
	})
}

// jobFromRow converts a generated row into a Job.
func jobFromRow(r pgdb.Job) (*Job, error) {
	j := &Job{
		ID: r.ID, TenantID: r.TenantID, Status: Status(r.Status), Spec: json.RawMessage(r.Spec),
		Backend: r.Backend, Priority: r.Priority, Attempt: r.Attempt, LostAttempts: r.LostAttempts,
		MaxLostAttempts: r.MaxLostAttempts, AvailableAt: r.AvailableAt, LeaseExpiresAt: r.LeaseExpiresAt,
		Score: r.Score, CreatedAt: r.CreatedAt, UpdatedAt: r.UpdatedAt, FinishedAt: r.FinishedAt,
		IdempotencyKey: deref(r.IdempotencyKey), AssignedNode: deref(r.AssignedNode), Error: deref(r.Error),
	}
	if len(r.Features) > 0 {
		if err := json.Unmarshal(r.Features, &j.Features); err != nil {
			return nil, fmt.Errorf("store: decode features of job %s: %w", r.ID, err)
		}
	}
	return j, nil
}

// deref returns the string p points to, or "".
func deref(p *string) string {
	if p == nil {
		return ""
	}
	return *p
}

// optional returns a pointer to s, or nil for "".
func optional(s string) *string {
	if s == "" {
		return nil
	}
	return &s
}

// seconds converts a positive duration to the float the queries take.
func seconds(d time.Duration, what string) (float64, error) {
	if d <= 0 {
		return 0, fmt.Errorf("%w: %s must be positive, got %s", ErrInvalid, what, d)
	}
	return d.Seconds(), nil
}
