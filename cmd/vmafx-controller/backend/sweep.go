// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/backend/sweep.go — the River job that expires leases
// and node sessions (ADR-2350 D2).

package backend

import (
	"context"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/riverqueue/river"

	"github.com/golusoris/golusoris/jobs"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store"
)

// LeaseSweepKind is the River job kind of the lease sweep.
const LeaseSweepKind = "vmafx_controller_lease_sweep"

// sweepTimeout bounds one sweep (HISS-02).
const sweepTimeout = 30 * time.Second

// LeaseSweepArgs is the lease sweep's River job; it carries no arguments.
type LeaseSweepArgs struct{}

// Kind implements river.JobArgs.
func (LeaseSweepArgs) Kind() string { return LeaseSweepKind }

// SweepReport is what one sweep did.
type SweepReport struct {
	Requeued        int
	Failed          int
	SessionsExpired int64
}

// LeaseSweeper is the River worker of the lease sweep: it returns jobs whose
// lease expired to pending (or fails them at their limit) and deletes expired
// sessions. River's elector inserts the periodic job once per cluster, any
// replica works it, and FOR UPDATE SKIP LOCKED keeps two sweeps apart.
type LeaseSweeper struct {
	river.WorkerDefaults[LeaseSweepArgs]
	store   *store.Postgres
	backoff store.Backoff
	report  func(SweepReport)
}

// NewLeaseSweeper returns the worker. report, when not nil, receives what
// every sweep did (the requeue metric of the controller).
func NewLeaseSweeper(st *store.Postgres, backoff store.Backoff, report func(SweepReport)) *LeaseSweeper {
	return &LeaseSweeper{store: st, backoff: backoff, report: report}
}

// Timeout implements river.Worker.
func (w *LeaseSweeper) Timeout(*river.Job[LeaseSweepArgs]) time.Duration { return sweepTimeout }

// Work implements river.Worker.
func (w *LeaseSweeper) Work(ctx context.Context, _ *river.Job[LeaseSweepArgs]) error {
	exp, err := w.store.ExpireLeases(ctx, w.backoff, store.MaxExpiryBatch)
	if err != nil {
		return fmt.Errorf("backend: lease sweep: %w", err)
	}
	gone, err := w.store.ExpireSessions(ctx)
	if err != nil {
		return fmt.Errorf("backend: session sweep: %w", err)
	}
	if w.report != nil {
		w.report(SweepReport{Requeued: exp.Requeued, Failed: exp.Failed, SessionsExpired: gone})
	}
	return nil
}

// RegisterLeaseSweep adds the sweep worker to workers. It fails when workers
// is nil or uninitialised, w is nil, or the sweep kind is already registered.
func RegisterLeaseSweep(workers *jobs.Workers, w *LeaseSweeper) error {
	if err := jobs.Register(workers, river.Worker[LeaseSweepArgs](w)); err != nil {
		return fmt.Errorf("backend: register lease sweep: %w", err)
	}
	return nil
}

// ScheduleLeaseSweep adds the periodic sweep to client, every interval and
// once at start. Only River's elected leader inserts periodic jobs, so one
// sweep per interval runs however many replicas schedule it.
func ScheduleLeaseSweep(client *river.Client[pgx.Tx], every time.Duration) {
	client.PeriodicJobs().Add(river.NewPeriodicJob(
		river.PeriodicInterval(every),
		func() (river.JobArgs, *river.InsertOpts) { return LeaseSweepArgs{}, nil },
		&river.PeriodicJobOpts{ID: LeaseSweepKind, RunOnStart: true},
	))
}

// ExponentialBackoff returns base * 2^(lost-1), capped at max: the delay
// before a job whose lease expired lost times may be claimed again.
func ExponentialBackoff(base, maxDelay time.Duration) store.Backoff {
	return func(lost int32) time.Duration {
		d := base
		for i := int32(1); i < lost && d < maxDelay; i++ {
			d *= 2
		}
		return min(d, maxDelay)
	}
}
