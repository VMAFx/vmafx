// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store/maintenance.go — lease expiry, session expiry,
// queue counts.

package store

import (
	"context"
	"fmt"
	"time"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store/pgdb"
)

// Backoff returns the delay before a job whose lease expired for the lost-th
// time may be claimed again (lost >= 1). The controller passes River's retry
// policy (ADR-2350 D2), so node work and River jobs back off alike.
type Backoff func(lost int32) time.Duration

// ExpiryReport counts what one ExpireLeases call did.
type ExpiryReport struct {
	Requeued int
	Failed   int
}

// ExpireLeases ends every running attempt whose lease expired, at most
// maxRows of them (1 to MaxExpiryBatch): the job returns to pending after
// backoff(lost attempts), or fails once max_lost_attempts leases expired.
// Concurrent calls skip each other's rows. It runs as maintenance, across
// tenants.
func (s *Postgres) ExpireLeases(ctx context.Context, backoff Backoff, maxRows int32) (ExpiryReport, error) {
	if backoff == nil || maxRows < 1 || maxRows > MaxExpiryBatch {
		return ExpiryReport{}, fmt.Errorf("%w: backoff and a batch of 1 to %d rows are required", ErrInvalid, MaxExpiryBatch)
	}
	var rep ExpiryReport
	err := s.inMaintenance(ctx, func(ctx context.Context, q *pgdb.Queries) error {
		rows, err := q.ExpiredLeases(ctx, maxRows)
		if err != nil {
			return fmt.Errorf("store: read expired leases: %w", err)
		}
		for _, row := range rows {
			failed, eerr := expireOne(ctx, q, row, backoff)
			if eerr != nil {
				return eerr
			}
			if failed {
				rep.Failed++
			} else {
				rep.Requeued++
			}
		}
		return nil
	})
	if err != nil {
		return ExpiryReport{}, err
	}
	return rep, nil
}

// expireOne ends one expired attempt and reports whether it failed the job.
func expireOne(ctx context.Context, q *pgdb.Queries, row pgdb.ExpiredLeasesRow, backoff Backoff) (bool, error) {
	lost := row.LostAttempts + 1
	reason := fmt.Sprintf("lease of attempt %d expired (%d of %d)", row.Attempt, lost, row.MaxLostAttempts)
	failed := lost >= row.MaxLostAttempts
	var err error
	if failed {
		err = q.FailExpired(ctx, pgdb.FailExpiredParams{Error: reason, ID: row.ID, Attempt: row.Attempt})
	} else {
		delay := backoff(lost)
		if delay < 0 {
			delay = 0
		}
		err = q.RequeueExpired(ctx, pgdb.RequeueExpiredParams{
			DelaySeconds: delay.Seconds(), ID: row.ID, Attempt: row.Attempt,
		})
	}
	if err != nil {
		return false, fmt.Errorf("store: expire attempt %d of job %s: %w", row.Attempt, row.ID, err)
	}
	return failed, endAttempt(ctx, q, row.ID, row.Attempt, "expired", reason)
}

// ExpireSessions deletes the sessions that expired and reports how many. The
// leases those sessions held expire on their own schedule (ExpireLeases).
func (s *Postgres) ExpireSessions(ctx context.Context) (int64, error) {
	var n int64
	err := s.inMaintenance(ctx, func(ctx context.Context, q *pgdb.Queries) error {
		var derr error
		n, derr = q.DeleteExpiredSessions(ctx)
		if derr != nil {
			return fmt.Errorf("store: delete expired sessions: %w", derr)
		}
		return nil
	})
	return n, err
}

// ActiveCount is the number of pending or running jobs of one backend.
type ActiveCount struct {
	Status  Status
	Backend string
	Jobs    int64
}

// CountActive returns the pending and running jobs per backend across
// tenants, for queue metrics and the autoscaler's view.
func (s *Postgres) CountActive(ctx context.Context) ([]ActiveCount, error) {
	var out []ActiveCount
	err := s.inMaintenance(ctx, func(ctx context.Context, q *pgdb.Queries) error {
		rows, err := q.CountActive(ctx)
		if err != nil {
			return fmt.Errorf("store: count active jobs: %w", err)
		}
		out = make([]ActiveCount, 0, len(rows))
		for _, r := range rows {
			out = append(out, ActiveCount{Status: Status(r.Status), Backend: r.Backend, Jobs: r.Jobs})
		}
		return nil
	})
	return out, err
}
