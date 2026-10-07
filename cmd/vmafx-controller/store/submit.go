// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store/submit.go — submission, reads and cancellation.

package store

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"reflect"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store/pgdb"
)

// DefaultMaxLostAttempts is the number of expired leases that fail a job when
// SubmitParams.MaxLostAttempts is zero.
const DefaultMaxLostAttempts = 3

// JobsChannel is the PostgreSQL notification channel a submission notifies,
// with the job's backend as payload, so waiting claims on every replica wake.
const JobsChannel = "vmafx_jobs"

// SubmitParams describes a job to enqueue.
type SubmitParams struct {
	TenantID string
	// IdempotencyKey, when set, makes the submission idempotent per tenant: a
	// repeat returns the first job instead of a new one.
	IdempotencyKey  string
	Spec            json.RawMessage
	Backend         string
	Priority        int32
	MaxLostAttempts int32
}

// Submit enqueues a job and returns it with created = true, or returns the
// job an earlier submission with the same idempotency key created, with
// created = false. A repeat whose spec, backend or priority differs from the
// first is refused with ErrIdempotencyConflict.
func (s *Postgres) Submit(ctx context.Context, p SubmitParams) (*Job, bool, error) {
	params, err := insertParams(p)
	if err != nil {
		return nil, false, err
	}
	var (
		job     *Job
		created bool
	)
	err = s.inTenant(ctx, p.TenantID, func(ctx context.Context, q *pgdb.Queries) error {
		row, ierr := q.InsertJob(ctx, params)
		if ierr == nil {
			created = true
			if nerr := q.NotifyJobs(ctx, p.Backend); nerr != nil {
				return fmt.Errorf("store: notify %s: %w", JobsChannel, nerr)
			}
			job, ierr = jobFromRow(row)
			return ierr
		}
		if !errors.Is(ierr, pgx.ErrNoRows) {
			return fmt.Errorf("store: insert job: %w", ierr)
		}
		job, ierr = existingJob(ctx, q, p)
		return ierr
	})
	if err != nil {
		return nil, false, err
	}
	return job, created, nil
}

// insertParams validates p and builds the insert of a new job.
func insertParams(p SubmitParams) (pgdb.InsertJobParams, error) {
	if !json.Valid(p.Spec) {
		return pgdb.InsertJobParams{}, fmt.Errorf("%w: spec is not valid JSON", ErrInvalid)
	}
	maxLost := p.MaxLostAttempts
	if maxLost == 0 {
		maxLost = DefaultMaxLostAttempts
	}
	if maxLost < 1 {
		return pgdb.InsertJobParams{}, fmt.Errorf("%w: max lost attempts %d", ErrInvalid, maxLost)
	}
	id, err := uuid.NewV7()
	if err != nil {
		return pgdb.InsertJobParams{}, fmt.Errorf("store: job id: %w", err)
	}
	return pgdb.InsertJobParams{
		ID: id, TenantID: p.TenantID, IdempotencyKey: optional(p.IdempotencyKey), Spec: p.Spec,
		Backend: p.Backend, Priority: p.Priority, MaxLostAttempts: maxLost,
	}, nil
}

// existingJob returns the job an idempotency key already names, refusing a
// repeat that describes a different job.
func existingJob(ctx context.Context, q *pgdb.Queries, p SubmitParams) (*Job, error) {
	row, err := q.GetJobByIdempotencyKey(ctx, pgdb.GetJobByIdempotencyKeyParams{
		TenantID: p.TenantID, IdempotencyKey: optional(p.IdempotencyKey),
	})
	if err != nil {
		return nil, fmt.Errorf("store: read job of idempotency key: %w", err)
	}
	same, err := sameJSON(row.Spec, p.Spec)
	if err != nil {
		return nil, err
	}
	if !same || row.Backend != p.Backend || row.Priority != p.Priority {
		return nil, fmt.Errorf("%w (job %s)", ErrIdempotencyConflict, row.ID)
	}
	return jobFromRow(row)
}

// sameJSON reports whether two JSON documents hold the same value (jsonb
// does not keep key order or spacing).
func sameJSON(a, b []byte) (bool, error) {
	var va, vb any
	if err := json.Unmarshal(a, &va); err != nil {
		return false, fmt.Errorf("store: decode stored spec: %w", err)
	}
	if err := json.Unmarshal(b, &vb); err != nil {
		return false, fmt.Errorf("%w: spec: %w", ErrInvalid, err)
	}
	return reflect.DeepEqual(va, vb), nil
}

// Get returns a job of tenantID, or ErrNotFound.
func (s *Postgres) Get(ctx context.Context, tenantID string, id uuid.UUID) (*Job, error) {
	var job *Job
	err := s.inTenant(ctx, tenantID, func(ctx context.Context, q *pgdb.Queries) error {
		row, gerr := q.GetJob(ctx, pgdb.GetJobParams{ID: id, TenantID: tenantID})
		if errors.Is(gerr, pgx.ErrNoRows) {
			return fmt.Errorf("%w: %s", ErrNotFound, id)
		}
		if gerr != nil {
			return fmt.Errorf("store: read job %s: %w", id, gerr)
		}
		job, gerr = jobFromRow(row)
		return gerr
	})
	return job, err
}

// List returns the jobs of tenantID in submission order, all of them or
// those in statuses, at most MaxListJobs.
func (s *Postgres) List(ctx context.Context, tenantID string, statuses []Status) ([]*Job, error) {
	names := make([]string, 0, len(statuses))
	for _, st := range statuses {
		names = append(names, string(st))
	}
	var jobs []*Job
	err := s.inTenant(ctx, tenantID, func(ctx context.Context, q *pgdb.Queries) error {
		rows, lerr := q.ListJobs(ctx, pgdb.ListJobsParams{TenantID: tenantID, Statuses: names, MaxRows: MaxListJobs})
		if lerr != nil {
			return fmt.Errorf("store: list jobs: %w", lerr)
		}
		jobs = make([]*Job, 0, len(rows))
		for _, row := range rows {
			j, cerr := jobFromRow(row)
			if cerr != nil {
				return cerr
			}
			jobs = append(jobs, j)
		}
		return nil
	})
	return jobs, err
}

// Cancel cancels a pending or running job of tenantID and reports whether it
// did. A job that is already terminal is left as it is (false, nil); a job
// that does not exist for the tenant is ErrNotFound. A running job's node
// learns it from its next heartbeat (Heartbeat's cancelled list) and its
// report changes nothing.
func (s *Postgres) Cancel(ctx context.Context, tenantID string, id uuid.UUID) (bool, error) {
	cancelled := false
	err := s.inTenant(ctx, tenantID, func(ctx context.Context, q *pgdb.Queries) error {
		row, err := q.CancelJob(ctx, pgdb.CancelJobParams{ID: id, TenantID: tenantID})
		if errors.Is(err, pgx.ErrNoRows) {
			return cancelNoop(ctx, q, tenantID, id)
		}
		if err != nil {
			return fmt.Errorf("store: cancel job %s: %w", id, err)
		}
		cancelled = true
		if Status(row.PreviousStatus) != StatusRunning {
			return nil
		}
		return endAttempt(ctx, q, id, row.Attempt, "cancelled", "cancelled")
	})
	return cancelled && err == nil, err
}

// cancelNoop tells a terminal job (nil) from a missing one (ErrNotFound).
func cancelNoop(ctx context.Context, q *pgdb.Queries, tenantID string, id uuid.UUID) error {
	_, err := q.GetJob(ctx, pgdb.GetJobParams{ID: id, TenantID: tenantID})
	if errors.Is(err, pgx.ErrNoRows) {
		return fmt.Errorf("%w: %s", ErrNotFound, id)
	}
	if err != nil {
		return fmt.Errorf("store: read job %s: %w", id, err)
	}
	return nil
}
