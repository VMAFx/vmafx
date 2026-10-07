// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/backend/postgres_test.go — the Postgres backend through
// its Backend methods, and the lease sweep on a real River client.

package backend_test

import (
	"context"
	"errors"
	"log/slog"
	"testing"
	"time"

	"github.com/golusoris/golusoris/jobs"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/backend"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store/storetest"
)

func newBackend(t *testing.T) (*backend.Postgres, storetest.DB) {
	t.Helper()
	db := storetest.New(t, storetest.Image)
	return backend.NewPostgres(db.Store, backend.PostgresOptions{}), db
}

var cpu = backend.Capability{Backends: []string{"cpu"}}

func TestPostgresBackendRunsAJobEndToEnd(t *testing.T) {
	t.Parallel()
	b, _ := newBackend(t)
	ctx := context.Background()
	id, err := b.Submit(ctx, "t1", backend.Scoring{Reference: "r.yuv", Distorted: "d.yuv", Model: "m", Backend: "cpu"})
	if err != nil {
		t.Fatalf("submit: %v", err)
	}
	sess, err := b.Register(ctx, "t1", "node-a", cpu)
	if err != nil {
		t.Fatalf("register: %v", err)
	}
	job, err := b.Pull(ctx, "t1", sess, cpu)
	if err != nil || job == nil || job.ID != id || job.Status != backend.StatusRunning || job.AssignedNode != "node-a" {
		t.Fatalf("pull: %+v %v", job, err)
	}
	if job.Scoring != (backend.Scoring{Reference: "r.yuv", Distorted: "d.yuv", Model: "m", Backend: "cpu"}) {
		t.Fatalf("scoring round trip: %+v", job.Scoring)
	}
	if ok, err := b.MayReport(ctx, "t1", sess, id); err != nil || !ok {
		t.Fatalf("partial report of a running job: %v %v", ok, err)
	}
	if rec, err := b.Report(ctx, "t1", sess, id, backend.Result{Score: 80, Features: map[string]float64{"adm2": 0.9}}); err != nil || !rec {
		t.Fatalf("report: %v %v", rec, err)
	}
	if rec, err := b.Report(ctx, "t1", sess, id, backend.Result{Score: 80}); err != nil || rec {
		t.Fatalf("retried report: recorded=%v err=%v", rec, err)
	}
	got, err := b.Get(ctx, "t1", id)
	if err != nil || got.Status != backend.StatusCompleted || got.Score != 80 || got.Features["adm2"] != 0.9 || got.AssignedNode != "node-a" {
		t.Fatalf("get: %+v %v", got, err)
	}
	if ok, err := b.MayReport(ctx, "t1", sess, id); err != nil || ok {
		t.Fatalf("partial report of a finished job: %v %v", ok, err)
	}
}

func TestPostgresBackendKeepsTenantsApart(t *testing.T) {
	t.Parallel()
	b, _ := newBackend(t)
	ctx := context.Background()
	id, err := b.Submit(ctx, "t1", backend.Scoring{Reference: "r", Distorted: "d"})
	if err != nil {
		t.Fatalf("submit: %v", err)
	}
	if _, err := b.Get(ctx, "t2", id); !errors.Is(err, backend.ErrNotFound) {
		t.Fatalf("get by another tenant: %v", err)
	}
	if _, err := b.Cancel(ctx, "t2", id); !errors.Is(err, backend.ErrNotFound) {
		t.Fatalf("cancel by another tenant: %v", err)
	}
	foreign, err := b.Register(ctx, "t2", "node-b", cpu)
	if err != nil {
		t.Fatalf("register: %v", err)
	}
	if job, err := b.Pull(ctx, "t2", foreign, cpu); err != nil || job != nil {
		t.Fatalf("another tenant's node pulled %+v (%v)", job, err)
	}
	if _, err := b.Heartbeat(ctx, "t1", foreign, nil); !errors.Is(err, backend.ErrInvalidSession) {
		t.Fatalf("session used under another tenant: %v", err)
	}
	if jobs, err := b.List(ctx, "t2", nil); err != nil || len(jobs) != 0 {
		t.Fatalf("list of another tenant: %d %v", len(jobs), err)
	}
}

func TestPostgresBackendRefusesWhatIsNotTheNodes(t *testing.T) {
	t.Parallel()
	b, _ := newBackend(t)
	ctx := context.Background()
	id, _ := b.Submit(ctx, "t1", backend.Scoring{Reference: "r", Distorted: "d"})
	runner, _ := b.Register(ctx, "t1", "runner", cpu)
	other, _ := b.Register(ctx, "t1", "other", cpu)
	if job, err := b.Pull(ctx, "t1", runner, cpu); err != nil || job == nil {
		t.Fatalf("pull: %+v %v", job, err)
	}
	if _, err := b.Report(ctx, "t1", other, id, backend.Result{Score: 1}); !errors.Is(err, backend.ErrNotAssigned) {
		t.Fatalf("report by a node that does not run the job: %v", err)
	}
	if _, err := b.Report(ctx, "t1", runner, "not-a-job", backend.Result{}); !errors.Is(err, backend.ErrNotAssigned) {
		t.Fatalf("report of a malformed job ID: %v", err)
	}
	if _, err := b.Report(ctx, "t1", backend.Session{NodeID: "nope", Token: "x"}, id, backend.Result{}); !errors.Is(err, backend.ErrInvalidSession) {
		t.Fatalf("report under a malformed session: %v", err)
	}
	wrongToken := runner
	wrongToken.Token = "forged"
	if _, err := b.Report(ctx, "t1", wrongToken, id, backend.Result{}); !errors.Is(err, backend.ErrInvalidSession) {
		t.Fatalf("report with a forged token: %v", err)
	}
	if _, err := b.Get(ctx, "t1", "not-a-job"); !errors.Is(err, backend.ErrNotFound) {
		t.Fatalf("get of a malformed ID: %v", err)
	}
}

func TestPostgresBackendHeartbeatNamesCancelledJobs(t *testing.T) {
	t.Parallel()
	b, _ := newBackend(t)
	ctx := context.Background()
	id, _ := b.Submit(ctx, "t1", backend.Scoring{Reference: "r", Distorted: "d"})
	sess, _ := b.Register(ctx, "t1", "n", cpu)
	if _, err := b.Pull(ctx, "t1", sess, cpu); err != nil {
		t.Fatalf("pull: %v", err)
	}
	if done, err := b.Cancel(ctx, "t1", id); err != nil || !done {
		t.Fatalf("cancel: %v %v", done, err)
	}
	cancelled, err := b.Heartbeat(ctx, "t1", sess, []string{"garbage", id})
	if err != nil || len(cancelled) != 1 || cancelled[0] != id {
		t.Fatalf("heartbeat: %v %v", cancelled, err)
	}
	st, err := b.Stats(ctx)
	if err != nil || st.LiveNodes != 1 || len(st.Tenants) != 0 {
		t.Fatalf("stats after the only job ended: %+v %v", st, err)
	}
	if err := b.Ready(ctx); err != nil {
		t.Fatalf("ready: %v", err)
	}
}

func TestLeaseSweepRunsOnRiverAndRequeuesALostJob(t *testing.T) {
	t.Parallel()
	b, db := newBackend(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	if err := jobs.Migrate(ctx, db.Pool); err != nil {
		t.Fatalf("river migrate: %v", err)
	}
	reports := make(chan backend.SweepReport, 16)
	workers := jobs.NewWorkers()
	backend.RegisterLeaseSweep(workers, backend.NewLeaseSweeper(db.Store, backend.ExponentialBackoff(0, 0),
		func(r backend.SweepReport) { b.RecordSweep(r); reports <- r }))
	client, err := jobs.New(db.Pool, jobs.DefaultOptions(), workers, slog.New(slog.DiscardHandler))
	if err != nil {
		t.Fatalf("river client: %v", err)
	}
	backend.ScheduleLeaseSweep(client, 200*time.Millisecond)
	id, _ := b.Submit(ctx, "t1", backend.Scoring{Reference: "r", Distorted: "d"})
	sess, _ := b.Register(ctx, "t1", "n", cpu)
	if _, err := b.Pull(ctx, "t1", sess, cpu); err != nil {
		t.Fatalf("pull: %v", err)
	}
	storetest.ExpireLease(t, db.Pool, id)
	if err := client.Start(ctx); err != nil {
		t.Fatalf("river start: %v", err)
	}
	t.Cleanup(func() {
		stopCtx, stop := context.WithTimeout(context.Background(), 10*time.Second)
		defer stop()
		_ = client.StopAndCancel(stopCtx)
	})
	if !requeued(ctx, reports) {
		t.Fatalf("no sweep requeued the job within the deadline")
	}
	if job, err := b.Get(ctx, "t1", id); err != nil || job.Status != backend.StatusPending || job.AssignedNode != "" {
		t.Fatalf("job after the sweep: %+v %v", job, err)
	}
	if st, err := b.Stats(ctx); err != nil || st.Requeued[backend.RequeueNodeLost] < 1 {
		t.Fatalf("requeue count after the sweep: %+v %v", st, err)
	}
}

// maxSweepReports bounds how many sweeps requeued waits through.
const maxSweepReports = 1000

// requeued waits for a sweep that requeued a job, or for ctx.
func requeued(ctx context.Context, reports <-chan backend.SweepReport) bool {
	for range maxSweepReports {
		select {
		case r := <-reports:
			if r.Requeued > 0 {
				return true
			}
		case <-ctx.Done():
			return false
		}
	}
	return false
}

func TestExponentialBackoffDoublesUpToTheCap(t *testing.T) {
	t.Parallel()
	f := backend.ExponentialBackoff(5*time.Second, time.Minute)
	want := map[int32]time.Duration{1: 5 * time.Second, 2: 10 * time.Second, 3: 20 * time.Second, 4: 40 * time.Second, 5: time.Minute, 30: time.Minute}
	for lost, d := range want {
		if got := f(lost); got != d {
			t.Errorf("backoff(%d) = %s, want %s", lost, got, d)
		}
	}
	var _ store.Backoff = f
}
