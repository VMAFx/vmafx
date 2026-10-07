// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/backend/postgres.go — the Backend on the PostgreSQL
// store (ADR-2350 D2).

package backend

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"sync/atomic"
	"time"

	"github.com/google/uuid"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store"
)

// PostgresOptions sets the lifetimes the Postgres backend grants.
type PostgresOptions struct {
	// LeaseTTL is how long a claim lasts without a heartbeat naming the job.
	LeaseTTL time.Duration
	// SessionTTL is how long a node session lasts without a heartbeat.
	SessionTTL time.Duration
}

// DefaultPostgresOptions match the node's heartbeat (every 10 s) and the
// eviction delay of the SQLite queue's registry (60 s).
func DefaultPostgresOptions() PostgresOptions {
	return PostgresOptions{LeaseTTL: 60 * time.Second, SessionTTL: 60 * time.Second}
}

// Postgres is the Backend on store.Postgres.
type Postgres struct {
	store *store.Postgres
	opts  PostgresOptions
	// requeued counts the jobs this process's sweeps returned to pending.
	requeued atomic.Uint64
}

var _ Backend = (*Postgres)(nil)

// NewPostgres returns the backend on st. Zero lifetimes in opts take the
// defaults.
func NewPostgres(st *store.Postgres, opts PostgresOptions) *Postgres {
	d := DefaultPostgresOptions()
	if opts.LeaseTTL <= 0 {
		opts.LeaseTTL = d.LeaseTTL
	}
	if opts.SessionTTL <= 0 {
		opts.SessionTTL = d.SessionTTL
	}
	return &Postgres{store: st, opts: opts}
}

// Submit implements Backend.
func (p *Postgres) Submit(ctx context.Context, tenantID string, s Scoring) (string, error) {
	spec, err := json.Marshal(s)
	if err != nil {
		return "", fmt.Errorf("backend: encode scoring: %w", err)
	}
	job, _, err := p.store.Submit(ctx, store.SubmitParams{TenantID: tenantID, Spec: spec, Backend: s.Backend})
	if err != nil {
		return "", fmt.Errorf("backend: submit: %w", err)
	}
	return job.ID.String(), nil
}

// Get implements Backend.
func (p *Postgres) Get(ctx context.Context, tenantID, jobID string) (*Job, error) {
	id, err := parseJobID(jobID)
	if err != nil {
		return nil, err
	}
	j, err := p.store.Get(ctx, tenantID, id)
	if err != nil {
		return nil, mapStoreErr(err)
	}
	return fromStoreJob(j)
}

// Cancel implements Backend.
func (p *Postgres) Cancel(ctx context.Context, tenantID, jobID string) (bool, error) {
	id, err := parseJobID(jobID)
	if err != nil {
		return false, err
	}
	done, err := p.store.Cancel(ctx, tenantID, id)
	return done, mapStoreErr(err)
}

// List implements Backend.
func (p *Postgres) List(ctx context.Context, tenantID string, statuses []string) ([]*Job, error) {
	want := make([]store.Status, 0, len(statuses))
	for _, s := range statuses {
		want = append(want, store.Status(s))
	}
	jobs, err := p.store.List(ctx, tenantID, want)
	if err != nil {
		return nil, mapStoreErr(err)
	}
	out := make([]*Job, 0, len(jobs))
	for _, j := range jobs {
		v, cerr := fromStoreJob(j)
		if cerr != nil {
			return nil, cerr
		}
		out = append(out, v)
	}
	return out, nil
}

// Register implements Backend. The node ID returned is the session's ID.
func (p *Postgres) Register(ctx context.Context, tenantID, name string, c Capability) (Session, error) {
	capability, err := json.Marshal(c)
	if err != nil {
		return Session{}, fmt.Errorf("backend: encode capability: %w", err)
	}
	sess, err := p.store.RegisterSession(ctx, store.RegisterParams{
		TenantID: tenantID, NodeID: name, Capability: capability, TTL: p.opts.SessionTTL,
	})
	if err != nil {
		return Session{}, mapStoreErr(err)
	}
	return Session{NodeID: sess.ID.String(), Token: sess.Token}, nil
}

// Heartbeat implements Backend. Running IDs that are not job IDs are ignored,
// as unknown IDs are.
func (p *Postgres) Heartbeat(ctx context.Context, tenantID string, s Session, running []string) ([]string, error) {
	ref, err := sessionRef(tenantID, s)
	if err != nil {
		return nil, err
	}
	ids := make([]uuid.UUID, 0, len(running))
	for _, r := range running {
		if id, perr := uuid.Parse(r); perr == nil {
			ids = append(ids, id)
		}
	}
	cancelled, err := p.store.Heartbeat(ctx, store.HeartbeatParams{
		Session: ref, Running: ids, SessionTTL: p.opts.SessionTTL, LeaseTTL: p.opts.LeaseTTL,
	})
	if err != nil {
		return nil, mapStoreErr(err)
	}
	out := make([]string, 0, len(cancelled))
	for _, id := range cancelled {
		out = append(out, id.String())
	}
	return out, nil
}

// Pull implements Backend.
func (p *Postgres) Pull(ctx context.Context, tenantID string, s Session, c Capability) (*Job, error) {
	ref, err := sessionRef(tenantID, s)
	if err != nil {
		return nil, err
	}
	claim, err := p.store.Claim(ctx, store.ClaimParams{Session: ref, Backends: c.Backends, LeaseTTL: p.opts.LeaseTTL})
	if err != nil {
		return nil, mapStoreErr(err)
	}
	if claim == nil {
		return nil, nil
	}
	return fromStoreJob(claim.Job)
}

// Report implements Backend. The attempt reported is the one the session
// holds, or held last (ADR-2350 D2).
func (p *Postgres) Report(ctx context.Context, tenantID string, s Session, jobID string, r Result) (bool, error) {
	ref, id, err := p.reportTarget(tenantID, s, jobID)
	if err != nil {
		return false, err
	}
	attempt, err := p.store.AttemptOf(ctx, ref, id)
	if err != nil {
		return false, mapStoreErr(err)
	}
	recorded, err := p.store.Report(ctx, store.AttemptRef{Session: ref, JobID: id, Attempt: attempt},
		store.Result{Score: r.Score, Features: r.Features, Err: r.Err})
	return recorded, mapStoreErr(err)
}

// MayReport implements Backend.
func (p *Postgres) MayReport(ctx context.Context, tenantID string, s Session, jobID string) (bool, error) {
	ref, id, err := p.reportTarget(tenantID, s, jobID)
	if err != nil {
		if errors.Is(err, ErrNotAssigned) {
			return false, nil
		}
		return false, err
	}
	_, held, err := p.store.RunningAttempt(ctx, ref, id)
	return held, mapStoreErr(err)
}

// reportTarget parses the session and job of a report; an ID that is not a
// job ID is a job the session does not run.
func (p *Postgres) reportTarget(tenantID string, s Session, jobID string) (store.SessionRef, uuid.UUID, error) {
	ref, err := sessionRef(tenantID, s)
	if err != nil {
		return store.SessionRef{}, uuid.Nil, err
	}
	id, err := uuid.Parse(jobID)
	if err != nil {
		return store.SessionRef{}, uuid.Nil, fmt.Errorf("%w: %q is not a job ID", ErrNotAssigned, jobID)
	}
	return ref, id, nil
}

// Stats implements Backend.
func (p *Postgres) Stats(ctx context.Context) (Stats, error) {
	st, err := p.store.Stats(ctx)
	if err != nil {
		return Stats{}, mapStoreErr(err)
	}
	out := Stats{
		LiveNodes: int(st.LiveNodes), Tenants: make([]TenantCount, 0, len(st.Tenants)),
		Requeued: map[string]uint64{RequeueNodeLost: p.requeued.Load()},
	}
	for _, t := range st.Tenants {
		out.Tenants = append(out.Tenants, TenantCount{
			TenantID: t.TenantID, Pending: int(t.Pending), Running: int(t.Running), OldestPending: t.OldestPending,
		})
	}
	return out, nil
}

// RecordSweep counts what a lease sweep of this process did; pass it as the
// report of NewLeaseSweeper.
func (p *Postgres) RecordSweep(r SweepReport) {
	if r.Requeued > 0 {
		p.requeued.Add(uint64(r.Requeued))
	}
}

// Ready implements Backend: the database answers and carries the schema
// this controller needs.
func (p *Postgres) Ready(ctx context.Context) error {
	if err := p.store.CheckSchema(ctx); err != nil {
		return fmt.Errorf("backend: %w", err)
	}
	return nil
}

// parseJobID parses a job ID; anything else names no job.
func parseJobID(jobID string) (uuid.UUID, error) {
	id, err := uuid.Parse(jobID)
	if err != nil {
		return uuid.Nil, fmt.Errorf("%w: %q", ErrNotFound, jobID)
	}
	return id, nil
}

// sessionRef parses a session; a node ID that is not a session ID names no
// session.
func sessionRef(tenantID string, s Session) (store.SessionRef, error) {
	id, err := uuid.Parse(s.NodeID)
	if err != nil {
		return store.SessionRef{}, fmt.Errorf("%w: node %q", ErrInvalidSession, s.NodeID)
	}
	return store.SessionRef{ID: id, TenantID: tenantID, Token: s.Token}, nil
}

// mapStoreErr maps the store's refusals onto the Backend errors.
func mapStoreErr(err error) error {
	switch {
	case err == nil:
		return nil
	case errors.Is(err, store.ErrNotFound):
		return fmt.Errorf("%w: %w", ErrNotFound, err)
	case errors.Is(err, store.ErrSessionInvalid):
		return fmt.Errorf("%w: %w", ErrInvalidSession, err)
	case errors.Is(err, store.ErrFenced):
		return fmt.Errorf("%w: %w", ErrNotAssigned, err)
	default:
		return err
	}
}

// fromStoreJob converts a store job into the controller's view.
func fromStoreJob(j *store.Job) (*Job, error) {
	var s Scoring
	if err := json.Unmarshal(j.Spec, &s); err != nil {
		return nil, fmt.Errorf("backend: decode spec of job %s: %w", j.ID, err)
	}
	v := &Job{
		ID: j.ID.String(), TenantID: j.TenantID, Status: string(j.Status), Scoring: s,
		AssignedNode: j.AssignedNode, Features: j.Features, Error: j.Error,
		CreatedAt: j.CreatedAt, UpdatedAt: j.UpdatedAt,
	}
	if j.Score != nil {
		v.Score = *j.Score
	}
	return v, nil
}
