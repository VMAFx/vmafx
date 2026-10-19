// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/backend/legacy.go — the Backend on the embedded SQLite
// queue and the in-memory node registry (ADR-1119, transitional per
// ADR-2350).

package backend

import (
	"context"
	"errors"
	"fmt"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/scheduler"
)

// Legacy is the Backend on the SQLite queue (one controller replica): jobs in
// queue.Queue, node sessions in nodes.Registry, assignment by
// scheduler.Scheduler. It keeps the behaviour those packages had behind the
// gRPC handlers: an orphaned job is adopted by a new session of its tenant
// (ADR-1524), and the queue answers for the tenant of every job (ADR-1522).
type Legacy struct {
	queue    queue.Queue
	registry *nodes.Registry
	sched    *scheduler.Scheduler
}

var _ Backend = (*Legacy)(nil)

// NewLegacy returns the backend on q, r and s.
func NewLegacy(q queue.Queue, r *nodes.Registry, s *scheduler.Scheduler) *Legacy {
	return &Legacy{queue: q, registry: r, sched: s}
}

// Submit implements Backend.
func (l *Legacy) Submit(ctx context.Context, tenantID string, s Scoring) (string, error) {
	return l.queue.Submit(ctx, &queue.Job{TenantID: tenantID, Scoring: queue.ScoringParams(s)})
}

// Get implements Backend.
func (l *Legacy) Get(ctx context.Context, tenantID, jobID string) (*Job, error) {
	j, err := l.queue.Get(ctx, jobID)
	if err != nil {
		return nil, fmt.Errorf("%w: %s: %w", ErrNotFound, jobID, err)
	}
	if j.TenantID != tenantID {
		return nil, fmt.Errorf("%w: %s", ErrForbidden, jobID)
	}
	return fromQueueJob(j), nil
}

// Cancel implements Backend.
func (l *Legacy) Cancel(ctx context.Context, tenantID, jobID string) (bool, error) {
	if _, err := l.Get(ctx, tenantID, jobID); err != nil {
		return false, err
	}
	return l.queue.Cancel(ctx, jobID)
}

// List implements Backend.
func (l *Legacy) List(ctx context.Context, tenantID string, statuses []string) ([]*Job, error) {
	jobs, err := l.queue.ListByTenant(ctx, tenantID, statuses)
	if err != nil {
		return nil, err
	}
	out := make([]*Job, 0, len(jobs))
	for _, j := range jobs {
		out = append(out, fromQueueJob(j))
	}
	return out, nil
}

// Register implements Backend.
func (l *Legacy) Register(_ context.Context, tenantID, name string, c Capability) (Session, error) {
	id, token, err := l.registry.Register(name, tenantID, nodes.Capability(c))
	if err != nil {
		return Session{}, err
	}
	return Session{NodeID: id, Token: token}, nil
}

// Heartbeat implements Backend.
func (l *Legacy) Heartbeat(ctx context.Context, tenantID string, s Session, running []string) ([]string, error) {
	if !l.registry.Heartbeat(s.NodeID, s.Token, tenantID, len(running)) {
		return nil, ErrInvalidSession
	}
	return l.queue.CancelledAmong(ctx, tenantID, running)
}

// Pull implements Backend.
func (l *Legacy) Pull(ctx context.Context, tenantID string, s Session, c Capability) (*Job, error) {
	j, err := l.sched.Assign(ctx, s.NodeID, s.Token, tenantID, nodes.Capability(c))
	if err != nil {
		return nil, fmt.Errorf("%w: %w", ErrInvalidSession, err)
	}
	if j == nil {
		return nil, nil
	}
	return fromQueueJob(j), nil
}

// Report implements Backend.
func (l *Legacy) Report(ctx context.Context, tenantID string, s Session, jobID string, r Result) (bool, error) {
	if !l.registry.ValidateSession(s.NodeID, s.Token, tenantID) {
		return false, ErrInvalidSession
	}
	rep := l.report(tenantID, s, jobID, &queue.JobResult{Score: r.Score, Features: r.Features, Err: r.Err})
	recorded, err := l.queue.ReportResult(ctx, rep)
	if errors.Is(err, queue.ErrNotAssigned) {
		return false, fmt.Errorf("%w: %w", ErrNotAssigned, err)
	}
	return recorded, err
}

// MayReport implements Backend.
func (l *Legacy) MayReport(ctx context.Context, tenantID string, s Session, jobID string) (bool, error) {
	if !l.registry.ValidateSession(s.NodeID, s.Token, tenantID) {
		return false, ErrInvalidSession
	}
	return l.queue.MayReport(ctx, l.report(tenantID, s, jobID, nil)), nil
}

// report builds the queue's report of a session's call.
func (l *Legacy) report(tenantID string, s Session, jobID string, r *queue.JobResult) queue.Report {
	return queue.Report{NodeID: s.NodeID, TenantID: tenantID, JobID: jobID, Result: r, Orphaned: l.orphaned}
}

// orphaned reports whether a node ID has no live session (ADR-1524).
func (l *Legacy) orphaned(nodeID string) bool {
	_, live := l.registry.Get(nodeID)
	return !live
}

// Stats implements Backend.
func (l *Legacy) Stats(ctx context.Context) (Stats, error) {
	st, err := l.queue.Stats(ctx)
	if err != nil {
		return Stats{}, err
	}
	out := Stats{LiveNodes: l.registry.Count(), Requeued: st.Requeued, Tenants: make([]TenantCount, 0, len(st.Tenants))}
	for _, t := range st.Tenants {
		out.Tenants = append(out.Tenants, TenantCount(t))
	}
	return out, nil
}

// Ready implements Backend: the queue answers a read.
func (l *Legacy) Ready(ctx context.Context) error {
	if _, err := l.queue.Stats(ctx); err != nil {
		return fmt.Errorf("backend: queue: %w", err)
	}
	return nil
}

// fromQueueJob converts a queue job into the controller's view.
func fromQueueJob(j *queue.Job) *Job {
	return &Job{
		ID: j.ID, TenantID: j.TenantID, Status: j.Status, Scoring: Scoring(j.Scoring),
		AssignedNode: j.AssignedNode, Score: j.Score, Features: j.Features, Error: j.Error,
		CreatedAt: j.CreatedAt, UpdatedAt: j.UpdatedAt,
	}
}
