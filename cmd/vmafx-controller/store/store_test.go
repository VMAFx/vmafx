// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store/store_test.go — submission, claims, fencing,
// heartbeats, cancellation, expiry and tenant isolation against PostgreSQL.

package store_test

import (
	"context"
	"errors"
	"log/slog"
	"sync"
	"testing"
	"time"

	"github.com/google/uuid"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store"
)

func TestMigrationsRunOnEverySupportedRelease(t *testing.T) {
	t.Parallel()
	for _, image := range []string{oldestImage, testImage} {
		t.Run(image, func(t *testing.T) {
			t.Parallel()
			db := newTestDB(t, image)
			ctx := context.Background()
			if err := db.store.CheckSchema(ctx); err != nil {
				t.Fatalf("CheckSchema after migrate: %v", err)
			}
			if err := store.MigratePostgres(db.dsn, slog.New(slog.DiscardHandler)); err != nil {
				t.Fatalf("second migrate must be a no-op: %v", err)
			}
			ref := register(t, db.store, "t1", "n1")
			id := submit(t, db.store, "t1", "cuda", 0)
			if c := claim(t, db.store, ref, "cuda"); c == nil || c.Job.ID != id {
				t.Fatalf("claim on %s: %+v", image, c)
			}
		})
	}
}

func TestCheckSchemaRefusesAnUnmigratedDatabase(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	if _, err := db.pool.Exec(ctx, "UPDATE schema_migrations SET dirty = true"); err != nil {
		t.Fatalf("mark dirty: %v", err)
	}
	if err := db.store.CheckSchema(ctx); !errors.Is(err, store.ErrSchemaOutdated) {
		t.Fatalf("dirty schema: err = %v, want ErrSchemaOutdated", err)
	}
	if _, err := db.pool.Exec(ctx, "DROP TABLE schema_migrations"); err != nil {
		t.Fatalf("drop bookkeeping: %v", err)
	}
	if err := db.store.CheckSchema(ctx); !errors.Is(err, store.ErrSchemaOutdated) {
		t.Fatalf("no bookkeeping table: err = %v, want ErrSchemaOutdated", err)
	}
}

func TestSubmitIsIdempotentPerTenant(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	p := store.SubmitParams{TenantID: "t1", IdempotencyKey: "k1", Spec: spec(t, "a"), Backend: "cuda"}
	first, created, err := db.store.Submit(ctx, p)
	if err != nil || !created {
		t.Fatalf("first submit: created=%v err=%v", created, err)
	}
	again, created, err := db.store.Submit(ctx, p)
	if err != nil || created || again.ID != first.ID {
		t.Fatalf("repeat: id=%s created=%v err=%v, want %s false nil", again.ID, created, err, first.ID)
	}
	changed := p
	changed.Spec = spec(t, "b")
	if _, _, err := db.store.Submit(ctx, changed); !errors.Is(err, store.ErrIdempotencyConflict) {
		t.Fatalf("same key, other spec: err = %v, want ErrIdempotencyConflict", err)
	}
	other := p
	other.TenantID = "t2"
	if j, created, err := db.store.Submit(ctx, other); err != nil || !created || j.ID == first.ID {
		t.Fatalf("same key, other tenant must be a new job: created=%v err=%v", created, err)
	}
}

func TestClaimHonoursTenantBackendAndOrder(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	foreign := submit(t, db.store, "t2", "cuda", 9)
	sycl := submit(t, db.store, "t1", "sycl", 9)
	old := submit(t, db.store, "t1", "cuda", 0)
	anywhere := submit(t, db.store, "t1", "", 0)
	urgent := submit(t, db.store, "t1", "cuda", 5)
	ref := register(t, db.store, "t1", "n1")
	want := []uuid.UUID{urgent, old, anywhere}
	for i, id := range want {
		c := claim(t, db.store, ref, "cuda", "cpu")
		if c == nil || c.Job.ID != id || c.Attempt != 1 {
			t.Fatalf("claim %d: got %+v, want job %s attempt 1", i, c, id)
		}
	}
	if c := claim(t, db.store, ref, "cuda", "cpu"); c != nil {
		t.Fatalf("claimed %s: neither another tenant's job (%s) nor a sycl job (%s) may be given", c.Job.ID, foreign, sycl)
	}
}

func TestConcurrentClaimsTakeEachJobOnce(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	const jobs, workers = 24, 6
	for range jobs {
		submit(t, db.store, "t1", "cpu", 0)
	}
	var (
		mu   sync.Mutex
		seen = map[uuid.UUID]int{}
		wg   sync.WaitGroup
	)
	for w := range workers {
		ref := register(t, db.store, "t1", "n"+string(rune('a'+w)))
		wg.Add(1)
		go func() {
			defer wg.Done()
			for range jobs {
				c, err := db.store.Claim(context.Background(), store.ClaimParams{Session: ref, Backends: []string{"cpu"}, LeaseTTL: time.Minute})
				if err != nil || c == nil {
					return
				}
				mu.Lock()
				seen[c.Job.ID]++
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	if len(seen) != jobs {
		t.Fatalf("claimed %d distinct jobs, want %d", len(seen), jobs)
	}
	for id, n := range seen {
		if n != 1 {
			t.Fatalf("job %s claimed %d times", id, n)
		}
	}
}

func TestFencingGivesOneResultAfterALostLease(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	id := submit(t, db.store, "t1", "cpu", 0)
	first := register(t, db.store, "t1", "n1")
	second := register(t, db.store, "t1", "n2")
	if c := claim(t, db.store, first, "cpu"); c == nil || c.Attempt != 1 {
		t.Fatalf("first claim: %+v", c)
	}
	expireLease(t, db, id)
	if rep, err := db.store.ExpireLeases(ctx, noBackoff, 10); err != nil || rep.Requeued != 1 {
		t.Fatalf("expire: %+v %v", rep, err)
	}
	if c := claim(t, db.store, second, "cpu"); c == nil || c.Attempt != 2 {
		t.Fatalf("second claim: %+v", c)
	}
	late := store.AttemptRef{Session: first, JobID: id, Attempt: 1}
	if err := db.store.Report(ctx, late, store.Result{Score: 1}); !errors.Is(err, store.ErrFenced) {
		t.Fatalf("report of the lost attempt: err = %v, want ErrFenced", err)
	}
	current := store.AttemptRef{Session: second, JobID: id, Attempt: 2}
	if err := db.store.Report(ctx, current, store.Result{Score: 91.5, Features: map[string]float64{"vif": 0.9}}); err != nil {
		t.Fatalf("report of the running attempt: %v", err)
	}
	if err := db.store.Report(ctx, current, store.Result{Score: 91.5}); err != nil {
		t.Fatalf("retried report must succeed without a write: %v", err)
	}
	job, err := db.store.Get(ctx, "t1", id)
	if err != nil || job.Status != store.StatusCompleted || *job.Score != 91.5 || job.Features["vif"] != 0.9 {
		t.Fatalf("job after reports: %+v err=%v", job, err)
	}
	if job.LostAttempts != 1 || job.Attempt != 2 {
		t.Fatalf("attempt bookkeeping: lost=%d attempt=%d, want 1 and 2", job.LostAttempts, job.Attempt)
	}
}

func TestLostLeasesFailTheJobAtTheLimit(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	job, _, err := db.store.Submit(ctx, store.SubmitParams{TenantID: "t1", Spec: spec(t, "x"), MaxLostAttempts: 2})
	if err != nil {
		t.Fatalf("submit: %v", err)
	}
	ref := register(t, db.store, "t1", "n1")
	for round := 1; round <= 2; round++ {
		if c := claim(t, db.store, ref); c == nil {
			t.Fatalf("round %d: nothing to claim", round)
		}
		expireLease(t, db, job.ID)
		if _, err := db.store.ExpireLeases(ctx, noBackoff, 10); err != nil {
			t.Fatalf("round %d expire: %v", round, err)
		}
	}
	got, err := db.store.Get(ctx, "t1", job.ID)
	if err != nil || got.Status != store.StatusFailed || got.LostAttempts != 2 || got.Error == "" {
		t.Fatalf("job after two lost leases: %+v err=%v", got, err)
	}
}

func TestExpiryBacksOffBeforeTheNextClaim(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	id := submit(t, db.store, "t1", "cpu", 0)
	ref := register(t, db.store, "t1", "n1")
	claim(t, db.store, ref, "cpu")
	expireLease(t, db, id)
	hour := func(int32) time.Duration { return time.Hour }
	if _, err := db.store.ExpireLeases(ctx, hour, 10); err != nil {
		t.Fatalf("expire: %v", err)
	}
	if c := claim(t, db.store, ref, "cpu"); c != nil {
		t.Fatalf("claimed %s inside its backoff", c.Job.ID)
	}
}

func TestReleaseReturnsTheJobWithoutCountingIt(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	id := submit(t, db.store, "t1", "cpu", 0)
	ref := register(t, db.store, "t1", "n1")
	claim(t, db.store, ref, "cpu")
	a := store.AttemptRef{Session: ref, JobID: id, Attempt: 1}
	if err := db.store.Release(ctx, a); err != nil {
		t.Fatalf("release: %v", err)
	}
	if err := db.store.Release(ctx, a); err != nil {
		t.Fatalf("repeated release must succeed: %v", err)
	}
	if err := db.store.Report(ctx, a, store.Result{Score: 1}); !errors.Is(err, store.ErrFenced) {
		t.Fatalf("report of a released attempt: err = %v, want ErrFenced", err)
	}
	c := claim(t, db.store, ref, "cpu")
	if c == nil || c.Attempt != 2 || c.Job.LostAttempts != 0 {
		t.Fatalf("claim after release: %+v", c)
	}
}

func TestHeartbeatRenewsLeasesAndNamesCancelledJobs(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	keep := submit(t, db.store, "t1", "cpu", 1)
	gone := submit(t, db.store, "t1", "cpu", 0)
	ref := register(t, db.store, "t1", "n1")
	first := claim(t, db.store, ref, "cpu")
	claim(t, db.store, ref, "cpu")
	if err := db.store.Cancel(ctx, "t1", gone); err != nil {
		t.Fatalf("cancel: %v", err)
	}
	hb := store.HeartbeatParams{Session: ref, Running: []uuid.UUID{gone, keep}, SessionTTL: time.Minute, LeaseTTL: time.Hour}
	cancelled, err := db.store.Heartbeat(ctx, hb)
	if err != nil || len(cancelled) != 1 || cancelled[0] != gone {
		t.Fatalf("heartbeat: cancelled=%v err=%v", cancelled, err)
	}
	job, err := db.store.Get(ctx, "t1", keep)
	if err != nil || !job.LeaseExpiresAt.After(first.Job.LeaseExpiresAt.Add(30*time.Minute)) {
		t.Fatalf("lease not renewed: %+v err=%v", job, err)
	}
	wrongToken := hb
	wrongToken.Session.Token = "not-the-token"
	if _, err := db.store.Heartbeat(ctx, wrongToken); !errors.Is(err, store.ErrSessionInvalid) {
		t.Fatalf("wrong token: err = %v, want ErrSessionInvalid", err)
	}
	foreign := hb
	foreign.Session.TenantID = "t2"
	if _, err := db.store.Heartbeat(ctx, foreign); !errors.Is(err, store.ErrSessionInvalid) {
		t.Fatalf("other tenant: err = %v, want ErrSessionInvalid", err)
	}
}

func TestCancelledRunningJobIgnoresItsReport(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	id := submit(t, db.store, "t1", "cpu", 0)
	ref := register(t, db.store, "t1", "n1")
	claim(t, db.store, ref, "cpu")
	if err := db.store.Cancel(ctx, "t1", id); err != nil {
		t.Fatalf("cancel: %v", err)
	}
	a := store.AttemptRef{Session: ref, JobID: id, Attempt: 1}
	if err := db.store.Report(ctx, a, store.Result{Err: "stopped by the controller"}); err != nil {
		t.Fatalf("report after cancel: %v", err)
	}
	if job, err := db.store.Get(ctx, "t1", id); err != nil || job.Status != store.StatusCancelled {
		t.Fatalf("status after report: %+v err=%v", job, err)
	}
	if err := db.store.Cancel(ctx, "t1", id); err != nil {
		t.Fatalf("cancel of a terminal job must be a no-op: %v", err)
	}
	if err := db.store.Cancel(ctx, "t2", id); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("cancel by another tenant: err = %v, want ErrNotFound", err)
	}
}

func TestRowLevelSecurityHidesOtherTenants(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	submit(t, db.store, "t1", "", 0)
	submit(t, db.store, "t2", "", 0)
	count := func(setting string) int {
		tx, err := db.pool.Begin(ctx)
		if err != nil {
			t.Fatalf("begin: %v", err)
		}
		defer func() { _ = tx.Rollback(ctx) }()
		if setting != "" {
			if _, err := tx.Exec(ctx, setting); err != nil {
				t.Fatalf("%s: %v", setting, err)
			}
		}
		var n int
		if err := tx.QueryRow(ctx, "SELECT count(*) FROM jobs").Scan(&n); err != nil {
			t.Fatalf("count: %v", err)
		}
		return n
	}
	cases := map[string]int{
		"": 0,
		"SELECT set_config('vmafx.tenant_id', 't1', true)":   1,
		"SELECT set_config('vmafx.maintenance', 'on', true)": 2,
	}
	for setting, want := range cases {
		if got := count(setting); got != want {
			t.Fatalf("jobs visible with %q: %d, want %d", setting, got, want)
		}
	}
}

func TestMaintenanceCountsAndExpiresSessions(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	submit(t, db.store, "t1", "cuda", 0)
	submit(t, db.store, "t2", "cuda", 0)
	ref := register(t, db.store, "t1", "n1")
	claim(t, db.store, ref, "cuda")
	counts, err := db.store.CountActive(ctx)
	if err != nil || len(counts) != 2 || counts[0].Jobs != 1 || counts[1].Jobs != 1 {
		t.Fatalf("counts: %+v err=%v", counts, err)
	}
	if _, err := db.pool.Exec(ctx,
		"DO $$ BEGIN PERFORM set_config('vmafx.maintenance', 'on', true); UPDATE node_sessions SET expires_at = now() - interval '1 second'; END $$"); err != nil {
		t.Fatalf("age sessions: %v", err)
	}
	if n, err := db.store.ExpireSessions(ctx); err != nil || n != 1 {
		t.Fatalf("expire sessions: n=%d err=%v", n, err)
	}
	if _, err := db.store.Claim(ctx, store.ClaimParams{Session: ref, LeaseTTL: time.Minute}); !errors.Is(err, store.ErrSessionInvalid) {
		t.Fatalf("claim on an expired session: err = %v, want ErrSessionInvalid", err)
	}
}

func TestStoreRefusesBadArguments(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	ref := register(t, db.store, "t1", "n1")
	tooMany := make([]uuid.UUID, store.MaxHeartbeatJobs+1)
	cases := map[string]error{
		"empty tenant": func() error {
			_, _, err := db.store.Submit(ctx, store.SubmitParams{Spec: spec(t, "x")})
			return err
		}(),
		"spec not JSON": func() error {
			_, _, err := db.store.Submit(ctx, store.SubmitParams{TenantID: "t1", Spec: []byte("{")})
			return err
		}(),
		"zero lease": func() error {
			_, err := db.store.Claim(ctx, store.ClaimParams{Session: ref})
			return err
		}(),
		"too many running jobs": func() error {
			_, err := db.store.Heartbeat(ctx, store.HeartbeatParams{Session: ref, Running: tooMany, SessionTTL: time.Minute, LeaseTTL: time.Minute})
			return err
		}(),
		"non-finite feature": db.store.Report(ctx, store.AttemptRef{Session: ref, JobID: uuid.New(), Attempt: 1},
			store.Result{Features: map[string]float64{"x": nan()}}),
		"no backoff": func() error {
			_, err := db.store.ExpireLeases(ctx, nil, 10)
			return err
		}(),
	}
	for name, err := range cases {
		if !errors.Is(err, store.ErrInvalid) {
			t.Errorf("%s: err = %v, want ErrInvalid", name, err)
		}
	}
}

func TestFencingRefusesAnOldAttemptOfTheSameSession(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	id := submit(t, db.store, "t1", "cpu", 0)
	ref := register(t, db.store, "t1", "n1")
	claim(t, db.store, ref, "cpu")
	expireLease(t, db, id)
	if _, err := db.store.ExpireLeases(ctx, noBackoff, 10); err != nil {
		t.Fatalf("expire: %v", err)
	}
	if c := claim(t, db.store, ref, "cpu"); c == nil || c.Attempt != 2 {
		t.Fatalf("reclaim by the same session: %+v", c)
	}
	old := store.AttemptRef{Session: ref, JobID: id, Attempt: 1}
	if err := db.store.Report(ctx, old, store.Result{Score: 1}); !errors.Is(err, store.ErrFenced) {
		t.Fatalf("report of attempt 1 while attempt 2 runs: err = %v, want ErrFenced", err)
	}
	if job, err := db.store.Get(ctx, "t1", id); err != nil || job.Status != store.StatusRunning {
		t.Fatalf("job after the fenced report: %+v err=%v", job, err)
	}
}
