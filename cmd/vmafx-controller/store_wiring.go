// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store_wiring.go — where the controller keeps jobs and
// node sessions: the embedded SQLite queue or the PostgreSQL store
// (ADR-2350).
//
// Configuration (koanf via golusoris/config, VMAFX_ prefix):
//
//	VMAFX_STORE_BACKEND            -> store.backend            sqlite (default) | postgres
//	VMAFX_DB_DSN                   -> db.dsn                   PostgreSQL connection string (postgres)
//	VMAFX_STORE_LEASE_TTL          -> store.lease_ttl          lease of a pulled job (default 60s)
//	VMAFX_STORE_SESSION_TTL        -> store.session_ttl        node session lifetime (default 60s)
//	VMAFX_STORE_SWEEP_INTERVAL     -> store.sweep_interval     lease sweep period (default 5s)
//	VMAFX_STORE_BACKOFF_BASE       -> store.backoff_base       delay after a lost lease (default 5s)
//	VMAFX_STORE_BACKOFF_MAX        -> store.backoff_max        cap of that delay (default 5m)

//go:build cgo

package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"sync/atomic"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
	"go.uber.org/fx"

	"github.com/golusoris/golusoris/core/config"
	"github.com/golusoris/golusoris/jobs"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/backend"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store"
)

// Backend names of store.backend.
const (
	backendSQLite   = "sqlite"
	backendPostgres = "postgres"
)

// storeOptions are the store.* settings.
type storeOptions struct {
	Backend       string
	LeaseTTL      time.Duration
	SessionTTL    time.Duration
	SweepInterval time.Duration
	BackoffBase   time.Duration
	BackoffMax    time.Duration
}

// storeKeys are the koanf keys of storeOptions.
type storeKeys struct {
	Backend       string        `koanf:"backend"`
	LeaseTTL      time.Duration `koanf:"lease_ttl"`
	SessionTTL    time.Duration `koanf:"session_ttl"`
	SweepInterval time.Duration `koanf:"sweep_interval"`
	BackoffBase   time.Duration `koanf:"backoff_base"`
	BackoffMax    time.Duration `koanf:"backoff_max"`
}

// loadStoreOptions reads store.* with its defaults and refuses an unknown
// backend or a negative duration.
func loadStoreOptions(cfg *config.Config) (storeOptions, error) {
	var k storeKeys
	if err := cfg.Unmarshal("store", &k); err != nil {
		return storeOptions{}, fmt.Errorf("read store.*: %w", err)
	}
	o := storeOptions{
		Backend:       k.Backend,
		LeaseTTL:      orDefault(k.LeaseTTL, 60*time.Second),
		SessionTTL:    orDefault(k.SessionTTL, 60*time.Second),
		SweepInterval: orDefault(k.SweepInterval, 5*time.Second),
		BackoffBase:   orDefault(k.BackoffBase, 5*time.Second),
		BackoffMax:    orDefault(k.BackoffMax, 5*time.Minute),
	}
	if o.Backend == "" {
		o.Backend = backendSQLite
	}
	if o.Backend != backendSQLite && o.Backend != backendPostgres {
		return storeOptions{}, fmt.Errorf("VMAFX_STORE_BACKEND: %q is neither %q nor %q", o.Backend, backendSQLite, backendPostgres)
	}
	for name, d := range map[string]time.Duration{
		"lease_ttl": k.LeaseTTL, "session_ttl": k.SessionTTL, "sweep_interval": k.SweepInterval,
		"backoff_base": k.BackoffBase, "backoff_max": k.BackoffMax,
	} {
		if d < 0 {
			return storeOptions{}, fmt.Errorf("store.%s: %s is negative", name, d)
		}
	}
	return o, nil
}

// orDefault is d, or def when d is unset.
func orDefault(d, def time.Duration) time.Duration {
	if d > 0 {
		return d
	}
	return def
}

// provideBackend opens the configured backend and binds its lifecycle.
func provideBackend(lc fx.Lifecycle, cfg *config.Config, log *slog.Logger) (backend.Backend, error) {
	opts, err := loadStoreOptions(cfg)
	if err != nil {
		return nil, err
	}
	if opts.Backend == backendPostgres {
		return providePostgresBackend(lc, cfg, opts, log)
	}
	q, err := provideJobQueue(lc, cfg, log)
	if err != nil {
		return nil, err
	}
	r := provideNodeRegistry(lc, q, log)
	return backend.NewLegacy(q, r, provideScheduler(q, r, log)), nil
}

// postgresConnectTimeout bounds the connection check at start (HISS-02).
const postgresConnectTimeout = 30 * time.Second

// providePostgresBackend connects to PostgreSQL, checks the schema, and runs
// River (the lease sweep) for as long as the controller runs.
func providePostgresBackend(lc fx.Lifecycle, cfg *config.Config, opts storeOptions, log *slog.Logger) (backend.Backend, error) {
	dsn := cfg.String("db.dsn")
	if dsn == "" {
		return nil, fmt.Errorf("VMAFX_STORE_BACKEND=postgres needs VMAFX_DB_DSN")
	}
	ctx, cancel := context.WithTimeout(context.Background(), postgresConnectTimeout)
	defer cancel()
	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		return nil, fmt.Errorf("connect to PostgreSQL: %w", err)
	}
	st := store.NewPostgres(pool)
	b := backend.NewPostgres(st, backend.PostgresOptions{LeaseTTL: opts.LeaseTTL, SessionTTL: opts.SessionTTL})
	client, err := newSweepClient(pool, st, b, opts, log)
	if err != nil {
		pool.Close()
		return nil, err
	}
	river := newRiverRunner(client, log)
	lc.Append(postgresHook(pool, river, log))
	return postgresBackend{Postgres: b, river: river}, nil
}

// postgresBackend adds River to the readiness of the PostgreSQL backend: a
// replica whose River does not run sweeps no leases.
type postgresBackend struct {
	*backend.Postgres
	river *riverRunner
}

// Ready implements backend.Backend.
func (p postgresBackend) Ready(ctx context.Context) error {
	if !p.river.running.Load() {
		return errRiverNotRunning
	}
	if err := p.Postgres.Ready(ctx); err != nil {
		return fmt.Errorf("postgres backend: %w", err)
	}
	return nil
}

// newSweepClient builds the River client that works the lease sweep.
func newSweepClient(pool *pgxpool.Pool, st *store.Postgres, b *backend.Postgres, opts storeOptions, log *slog.Logger) (*jobs.Client, error) {
	workers := jobs.NewWorkers()
	backoff := backend.ExponentialBackoff(opts.BackoffBase, opts.BackoffMax)
	if err := backend.RegisterLeaseSweep(workers, backend.NewLeaseSweeper(st, backoff, b.RecordSweep)); err != nil {
		return nil, fmt.Errorf("start River: %w", err)
	}
	client, err := jobs.New(pool, jobs.DefaultOptions(), workers, log)
	if err != nil {
		return nil, fmt.Errorf("start River: %w", err)
	}
	backend.ScheduleLeaseSweep(client, opts.SweepInterval)
	return client, nil
}

// River start: a controller started before its database or before the
// migration Job keeps trying in the background and stays unready, every
// riverStartEvery, at most riverStartTries times (HISS-02; 30 minutes).
const (
	riverStartEvery = 2 * time.Second
	riverStartTries = 900
)

// errRiverNotRunning is the readiness answer until River runs.
var errRiverNotRunning = errors.New("not ready: River has not started yet (the database or its schema is not ready)")

// riverRunner runs River for the controller's lifetime. River keeps the
// context it is started with, and the context fx hands a start hook expires
// with the start timeout, so River gets its own, ended after River stops.
type riverRunner struct {
	client     *jobs.Client
	log        *slog.Logger
	running    atomic.Bool
	riverCtx   context.Context
	stopRiver  context.CancelFunc
	tryCtx     context.Context
	stopTrying context.CancelFunc
	done       chan struct{}
}

func newRiverRunner(client *jobs.Client, log *slog.Logger) *riverRunner {
	return &riverRunner{client: client, log: log, done: make(chan struct{})}
}

// start begins the background start of River and returns at once. Both
// contexts live as long as the controller: stop ends them.
func (r *riverRunner) start() {
	r.riverCtx, r.stopRiver = context.WithCancel(context.Background())
	r.tryCtx, r.stopTrying = context.WithCancel(context.Background())
	go r.run()
}

// run starts River, retrying until it starts, the controller stops, or the
// tries run out.
func (r *riverRunner) run() {
	defer close(r.done)
	t := time.NewTimer(0)
	defer t.Stop()
	for try := 1; try <= riverStartTries; try++ {
		select {
		case <-r.tryCtx.Done():
			return
		case <-t.C:
		}
		err := r.client.Start(r.riverCtx)
		if err == nil {
			r.running.Store(true)
			r.log.Info("River started", "try", try)
			return
		}
		r.log.Warn("River did not start; retrying", "try", try, "in", riverStartEvery, "error", err)
		t.Reset(riverStartEvery)
	}
	r.log.Error("River did not start; this controller stays unready", "tries", riverStartTries)
}

// stop ends the start loop, then stops River gracefully within ctx.
func (r *riverRunner) stop(ctx context.Context) error {
	r.stopTrying()
	select {
	case <-r.done:
	case <-ctx.Done():
		r.stopRiver()
		return fmt.Errorf("stop River: %w", ctx.Err())
	}
	defer r.stopRiver()
	if !r.running.Load() {
		return nil
	}
	if err := r.client.Stop(ctx); err != nil {
		return fmt.Errorf("stop River: %w", err)
	}
	return nil
}

// postgresHook starts River with the controller, in the background (see
// riverRunner), and stops it, then the pool, when the controller stops. The
// schema is checked by /readyz, not here, so a controller started before its
// database or its migration Job waits unready instead of exiting.
func postgresHook(pool *pgxpool.Pool, river *riverRunner, log *slog.Logger) fx.Hook {
	return fx.Hook{
		OnStart: func(context.Context) error {
			river.start()
			return nil
		},
		OnStop: func(ctx context.Context) error {
			log.Info("stopping River and closing the PostgreSQL pool")
			err := river.stop(ctx)
			pool.Close()
			return err
		},
	}
}
