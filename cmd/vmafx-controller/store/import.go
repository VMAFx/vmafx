// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store/import.go — jobs carried over from the embedded
// SQLite queue (ADR-2350 D16).

package store

import (
	"context"
	"encoding/json"
	"fmt"
	"time"

	"github.com/google/uuid"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store/pgdb"
)

// ImportedJob is a job carried over from another store, with its own ID and
// times. A pending or running job arrives pending, with no attempt; a
// finished one keeps its result.
type ImportedJob struct {
	ID           uuid.UUID
	TenantID     string
	Status       Status
	Spec         json.RawMessage
	Backend      string
	AssignedNode string
	Score        *float64
	Features     map[string]float64
	Error        string
	CreatedAt    time.Time
	UpdatedAt    time.Time
}

// Import writes j unless a job with its ID exists, and reports whether it
// did. It runs as maintenance: an import carries every tenant.
func (s *Postgres) Import(ctx context.Context, j ImportedJob) (bool, error) {
	params, err := importParams(j)
	if err != nil {
		return false, err
	}
	var n int64
	err = s.inMaintenance(ctx, func(ctx context.Context, q *pgdb.Queries) error {
		var ierr error
		n, ierr = q.ImportJob(ctx, params)
		if ierr != nil {
			return fmt.Errorf("store: import job %s: %w", j.ID, ierr)
		}
		return nil
	})
	return n == 1, err
}

// importParams validates j and builds its insert.
func importParams(j ImportedJob) (pgdb.ImportJobParams, error) {
	if j.TenantID == "" {
		return pgdb.ImportJobParams{}, fmt.Errorf("%w: job %s has no tenant", ErrInvalid, j.ID)
	}
	if !json.Valid(j.Spec) {
		return pgdb.ImportJobParams{}, fmt.Errorf("%w: job %s: spec is not valid JSON", ErrInvalid, j.ID)
	}
	p := pgdb.ImportJobParams{
		ID: j.ID, TenantID: j.TenantID, Spec: j.Spec, Backend: j.Backend,
		CreatedAt: j.CreatedAt, UpdatedAt: j.UpdatedAt,
	}
	switch j.Status {
	case StatusPending, StatusRunning:
		p.Status = string(StatusPending)
		return p, nil
	case StatusCompleted, StatusFailed, StatusCancelled:
	default:
		return pgdb.ImportJobParams{}, fmt.Errorf("%w: job %s: status %q", ErrInvalid, j.ID, j.Status)
	}
	features, err := json.Marshal(j.Features)
	if err != nil {
		return pgdb.ImportJobParams{}, fmt.Errorf("%w: job %s: features: %w", ErrInvalid, j.ID, err)
	}
	finished := j.UpdatedAt
	p.Status, p.Score, p.Features, p.FinishedAt = string(j.Status), j.Score, features, &finished
	p.AssignedNode, p.Error = optional(j.AssignedNode), optional(j.Error)
	return p, nil
}
