// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store/leases.go — leased claims, reports and releases.

package store

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store/pgdb"
)

// ClaimParams asks for the next job a session may run.
type ClaimParams struct {
	Session  SessionRef
	Backends []string
	LeaseTTL time.Duration
}

// Claim is a job leased to a session: Attempt is the fencing token every
// later write of the attempt names.
type Claim struct {
	Job     *Job
	Attempt int32
}

// Claim leases the next pending job of the session's tenant whose backend
// the session can run (an empty backend runs anywhere): highest priority
// first, then oldest. It returns nil when nothing is ready. Concurrent claims
// never get the same job (FOR UPDATE SKIP LOCKED).
func (s *Postgres) Claim(ctx context.Context, p ClaimParams) (*Claim, error) {
	if len(p.Backends) > MaxBackends {
		return nil, fmt.Errorf("%w: %d backends, at most %d", ErrInvalid, len(p.Backends), MaxBackends)
	}
	leaseTTL, err := seconds(p.LeaseTTL, "lease TTL")
	if err != nil {
		return nil, err
	}
	backends := p.Backends
	if backends == nil {
		backends = []string{}
	}
	var claim *Claim
	err = s.inTenant(ctx, p.Session.TenantID, func(ctx context.Context, q *pgdb.Queries) error {
		sess, serr := liveSession(ctx, q, p.Session)
		if serr != nil {
			return serr
		}
		claim, serr = claimNext(ctx, q, sess, backends, leaseTTL)
		return serr
	})
	return claim, err
}

// claimNext leases one job to sess and records the attempt.
func claimNext(ctx context.Context, q *pgdb.Queries, sess pgdb.NodeSession, backends []string, leaseTTL float64) (*Claim, error) {
	row, err := q.ClaimNext(ctx, pgdb.ClaimNextParams{
		TenantID: sess.TenantID, Backends: backends, SessionID: sess.ID, NodeID: sess.NodeID, LeaseSeconds: leaseTTL,
	})
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("store: claim job: %w", err)
	}
	if err := q.InsertAttempt(ctx, pgdb.InsertAttemptParams{
		JobID: row.ID, Attempt: row.Attempt, TenantID: sess.TenantID, SessionID: sess.ID, NodeID: sess.NodeID,
	}); err != nil {
		return nil, fmt.Errorf("store: record attempt %d of job %s: %w", row.Attempt, row.ID, err)
	}
	job, err := jobFromRow(row)
	if err != nil {
		return nil, err
	}
	return &Claim{Job: job, Attempt: row.Attempt}, nil
}

// AttemptRef names one attempt of a job, as its session reports it.
type AttemptRef struct {
	Session SessionRef
	JobID   uuid.UUID
	Attempt int32
}

// Result is the outcome a node reports; a non-empty Err fails the job.
type Result struct {
	Score    float64
	Features map[string]float64
	Err      string
}

// Report records the result of a running attempt and ends the job: completed,
// or failed when r.Err is set. It reports whether this call recorded the
// result. Only the session that holds the attempt's lease can report it.
// Repeating a report of the caller's own attempt after the job ended (a retry,
// or a report after a cancellation) is a no-op success (false, nil);
// reporting any other attempt is ErrFenced, and nothing is written.
func (s *Postgres) Report(ctx context.Context, a AttemptRef, r Result) (bool, error) {
	params, err := finishParams(a, r)
	if err != nil {
		return false, err
	}
	recorded := false
	err = s.inTenant(ctx, a.Session.TenantID, func(ctx context.Context, q *pgdb.Queries) error {
		if _, serr := liveSession(ctx, q, a.Session); serr != nil {
			return serr
		}
		n, ferr := q.FinishAttempt(ctx, params)
		if ferr != nil {
			return fmt.Errorf("store: report attempt %d of job %s: %w", a.Attempt, a.JobID, ferr)
		}
		if n == 0 {
			return endedOwnAttempt(ctx, q, a, reportRepeatable)
		}
		recorded = true
		return endAttempt(ctx, q, a.JobID, a.Attempt, params.Status, r.Err)
	})
	return recorded && err == nil, err
}

// finishParams builds the write that ends a job with r.
func finishParams(a AttemptRef, r Result) (pgdb.FinishAttemptParams, error) {
	features, err := json.Marshal(r.Features)
	if err != nil {
		// Only non-finite floats fail to marshal; refuse instead of dropping them.
		return pgdb.FinishAttemptParams{}, fmt.Errorf("%w: features of job %s: %w", ErrInvalid, a.JobID, err)
	}
	p := pgdb.FinishAttemptParams{
		Status: string(StatusCompleted), Score: &r.Score, Features: features,
		ID: a.JobID, TenantID: a.Session.TenantID, Attempt: a.Attempt, SessionID: a.Session.ID,
	}
	if r.Err != "" {
		p.Status, p.Error = string(StatusFailed), &r.Err
	}
	return p, nil
}

// Release returns a running attempt's job to pending at once, without
// counting it as lost (a draining node gives its work back). Releasing an
// attempt that already ended is a no-op success; any other attempt is
// ErrFenced.
func (s *Postgres) Release(ctx context.Context, a AttemptRef) error {
	return s.inTenant(ctx, a.Session.TenantID, func(ctx context.Context, q *pgdb.Queries) error {
		if _, serr := liveSession(ctx, q, a.Session); serr != nil {
			return serr
		}
		n, err := q.ReleaseAttempt(ctx, pgdb.ReleaseAttemptParams{
			ID: a.JobID, TenantID: a.Session.TenantID, Attempt: a.Attempt, SessionID: a.Session.ID,
		})
		if err != nil {
			return fmt.Errorf("store: release attempt %d of job %s: %w", a.Attempt, a.JobID, err)
		}
		if n == 0 {
			return endedOwnAttempt(ctx, q, a, releaseRepeatable)
		}
		if err := q.NotifyJobs(ctx, ""); err != nil {
			return fmt.Errorf("store: notify %s: %w", JobsChannel, err)
		}
		return endAttempt(ctx, q, a.JobID, a.Attempt, "released", "")
	})
}

// Outcomes after which a repeated write of the caller's own attempt is a
// no-op success: a retried report, a report or release after a cancellation,
// a retried release. A report of a released or expired attempt is fenced: its
// result was not recorded.
func reportRepeatable(outcome string) bool {
	return outcome == "completed" || outcome == "failed" || outcome == "cancelled"
}

func releaseRepeatable(outcome string) bool {
	return outcome == "released" || outcome == "cancelled"
}

// endedOwnAttempt decides a write that matched no running attempt: nil when
// a is the caller's own attempt and it ended with an outcome in repeatable;
// otherwise ErrNotFound (no such job of the tenant) or ErrFenced.
func endedOwnAttempt(ctx context.Context, q *pgdb.Queries, a AttemptRef, repeatable func(string) bool) error {
	if _, err := q.GetJob(ctx, pgdb.GetJobParams{ID: a.JobID, TenantID: a.Session.TenantID}); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return fmt.Errorf("%w: %s", ErrNotFound, a.JobID)
		}
		return fmt.Errorf("store: read job %s: %w", a.JobID, err)
	}
	att, err := q.GetAttempt(ctx, pgdb.GetAttemptParams{JobID: a.JobID, Attempt: a.Attempt, TenantID: a.Session.TenantID})
	if errors.Is(err, pgx.ErrNoRows) {
		return fmt.Errorf("%w: job %s has no attempt %d", ErrFenced, a.JobID, a.Attempt)
	}
	if err != nil {
		return fmt.Errorf("store: read attempt %d of job %s: %w", a.Attempt, a.JobID, err)
	}
	if att.SessionID == a.Session.ID && repeatable(deref(att.Outcome)) {
		return nil
	}
	return fmt.Errorf("%w: job %s attempt %d", ErrFenced, a.JobID, a.Attempt)
}

// endAttempt records how an attempt ended.
func endAttempt(ctx context.Context, q *pgdb.Queries, job uuid.UUID, attempt int32, outcome, reason string) error {
	if err := q.EndAttempt(ctx, pgdb.EndAttemptParams{
		Outcome: outcome, Error: optional(reason), JobID: job, Attempt: attempt,
	}); err != nil {
		return fmt.Errorf("store: end attempt %d of job %s: %w", attempt, job, err)
	}
	return nil
}

// AttemptOf returns the attempt of jobID the session should name in a write:
// the running attempt it holds, else the last attempt it ever held of the job
// (so a retried write after the job ended is decided by Report or Release).
// A job of the tenant that the session never held is ErrFenced; a job the
// tenant does not have is ErrNotFound.
func (s *Postgres) AttemptOf(ctx context.Context, ref SessionRef, jobID uuid.UUID) (int32, error) {
	var attempt int32
	err := s.inTenant(ctx, ref.TenantID, func(ctx context.Context, q *pgdb.Queries) error {
		if _, serr := liveSession(ctx, q, ref); serr != nil {
			return serr
		}
		var lerr error
		attempt, lerr = lookupAttempt(ctx, q, ref, jobID)
		return lerr
	})
	return attempt, err
}

// lookupAttempt is AttemptOf inside a tenant transaction.
func lookupAttempt(ctx context.Context, q *pgdb.Queries, ref SessionRef, jobID uuid.UUID) (int32, error) {
	running, err := q.RunningAttemptOfSession(ctx, pgdb.RunningAttemptOfSessionParams{
		ID: jobID, TenantID: ref.TenantID, SessionID: ref.ID,
	})
	if err == nil {
		return running, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		return 0, fmt.Errorf("store: read running attempt of job %s: %w", jobID, err)
	}
	latest, err := q.LatestAttemptOfSession(ctx, pgdb.LatestAttemptOfSessionParams{
		JobID: jobID, TenantID: ref.TenantID, SessionID: ref.ID,
	})
	if err == nil {
		return latest, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		return 0, fmt.Errorf("store: read attempts of job %s: %w", jobID, err)
	}
	if _, gerr := q.GetJob(ctx, pgdb.GetJobParams{ID: jobID, TenantID: ref.TenantID}); errors.Is(gerr, pgx.ErrNoRows) {
		return 0, fmt.Errorf("%w: %s", ErrNotFound, jobID)
	} else if gerr != nil {
		return 0, fmt.Errorf("store: read job %s: %w", jobID, gerr)
	}
	return 0, fmt.Errorf("%w: job %s was never leased to this session", ErrFenced, jobID)
}

// RunningAttempt returns the attempt of jobID the session holds a running
// lease on, and whether it holds one. It writes nothing.
func (s *Postgres) RunningAttempt(ctx context.Context, ref SessionRef, jobID uuid.UUID) (int32, bool, error) {
	var (
		attempt int32
		held    bool
	)
	err := s.inTenant(ctx, ref.TenantID, func(ctx context.Context, q *pgdb.Queries) error {
		if _, serr := liveSession(ctx, q, ref); serr != nil {
			return serr
		}
		a, rerr := q.RunningAttemptOfSession(ctx, pgdb.RunningAttemptOfSessionParams{
			ID: jobID, TenantID: ref.TenantID, SessionID: ref.ID,
		})
		if errors.Is(rerr, pgx.ErrNoRows) {
			return nil
		}
		if rerr != nil {
			return fmt.Errorf("store: read running attempt of job %s: %w", jobID, rerr)
		}
		attempt, held = a, true
		return nil
	})
	return attempt, held, err
}
