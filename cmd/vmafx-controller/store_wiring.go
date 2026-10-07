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
	"fmt"
	"log/slog"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
	"go.uber.org/fx"

	"github.com/golusoris/golusoris/core/config"
	"github.com/golusoris/golusoris/jobs"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/backend"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store"
)

// storeConfigKeys are the underscore-bearing leaf keys of this file; they are
// declared as CompoundKeys so the env transform keeps their underscores.
var storeConfigKeys = []string{
	"store.lease_ttl", "store.session_ttl", "store.sweep_interval",
	"store.backoff_base", "store.backoff_max",
}

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
	river, err := newSweepClient(pool, st, b, opts, log)
	if err != nil {
		pool.Close()
		return nil, err
	}
	lc.Append(postgresHook(pool, river, log))
	return b, nil
}

// newSweepClient builds the River client that works the lease sweep.
func newSweepClient(pool *pgxpool.Pool, st *store.Postgres, b *backend.Postgres, opts storeOptions, log *slog.Logger) (*jobs.Client, error) {
	workers := jobs.NewWorkers()
	backoff := backend.ExponentialBackoff(opts.BackoffBase, opts.BackoffMax)
	backend.RegisterLeaseSweep(workers, backend.NewLeaseSweeper(st, backoff, b.RecordSweep))
	client, err := jobs.New(pool, jobs.DefaultOptions(), workers, log)
	if err != nil {
		return nil, fmt.Errorf("start River: %w", err)
	}
	backend.ScheduleLeaseSweep(client, opts.SweepInterval)
	return client, nil
}

// postgresHook starts River with the controller and stops it, then the pool,
// when the controller stops. The schema is checked by /readyz, not here, so a
// controller started before its migration Job waits unready instead of
// crashing.
func postgresHook(pool *pgxpool.Pool, river *jobs.Client, log *slog.Logger) fx.Hook {
	return fx.Hook{
		OnStart: func(ctx context.Context) error {
			if err := river.Start(ctx); err != nil {
				return fmt.Errorf("start River: %w", err)
			}
			return nil
		},
		OnStop: func(ctx context.Context) error {
			log.Info("stopping River and closing the PostgreSQL pool")
			err := river.Stop(ctx)
			pool.Close()
			if err != nil {
				return fmt.Errorf("stop River: %w", err)
			}
			return nil
		},
	}
}
