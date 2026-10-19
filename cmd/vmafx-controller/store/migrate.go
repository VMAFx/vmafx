// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store/migrate.go — schema migrations and the schema
// version check.

package store

import (
	"context"
	"embed"
	"errors"
	"fmt"
	"log/slog"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"

	dbmigrate "github.com/golusoris/golusoris/db/migrate"
	dbpgx "github.com/golusoris/golusoris/db/pgx"
)

// SchemaVersion is the migration the store needs. Raise it with every new
// file under migrations/postgres/.
const SchemaVersion = 1

// postgresMigrationsDir is where the PostgreSQL migrations live in
// postgresMigrations.
const postgresMigrationsDir = "migrations/postgres"

//go:embed migrations/postgres/*.sql
var postgresMigrations embed.FS

// ErrSchemaOutdated is a database below SchemaVersion, or one a failed
// migration left dirty.
var ErrSchemaOutdated = errors.New("store: database schema is not the one this controller needs")

// MigratePostgres applies the store's migrations to the database at dsn
// (golusoris db/migrate). The controller's migration Job runs it; it is safe
// to run again (no change is success) and golang-migrate locks against a
// concurrent run.
func MigratePostgres(dsn string, logger *slog.Logger) (err error) {
	opts := dbmigrate.Options{Path: postgresMigrationsDir}.WithFS(postgresMigrations)
	m, err := dbmigrate.New(opts, dbpgx.Options{DSN: dsn}, logger)
	if err != nil {
		return fmt.Errorf("store: open migrations: %w", err)
	}
	defer func() {
		if cerr := m.Close(); cerr != nil && err == nil {
			err = fmt.Errorf("store: close migrations: %w", cerr)
		}
	}()
	if err := m.Up(); err != nil {
		return fmt.Errorf("store: migrate: %w", err)
	}
	return nil
}

// schemaVersionQuery reads golang-migrate's bookkeeping table.
const schemaVersionQuery = `SELECT version, dirty FROM schema_migrations LIMIT 1`

// CheckSchema returns nil when the database carries SchemaVersion or a newer
// migration and is not dirty, ErrSchemaOutdated (naming both versions)
// otherwise. A controller reports not ready until it passes.
func (s *Postgres) CheckSchema(ctx context.Context) error {
	var (
		version int64
		dirty   bool
	)
	err := s.pool.QueryRow(ctx, schemaVersionQuery).Scan(&version, &dirty)
	switch {
	case errors.Is(err, pgx.ErrNoRows):
		return fmt.Errorf("%w: no migration applied, need %d", ErrSchemaOutdated, SchemaVersion)
	case err != nil && isUndefinedTable(err):
		return fmt.Errorf("%w: no migration applied, need %d", ErrSchemaOutdated, SchemaVersion)
	case err != nil:
		return fmt.Errorf("store: read schema version: %w", err)
	case dirty:
		return fmt.Errorf("%w: migration %d failed part-way (dirty)", ErrSchemaOutdated, version)
	case version < SchemaVersion:
		return fmt.Errorf("%w: at %d, need %d", ErrSchemaOutdated, version, SchemaVersion)
	}
	return nil
}

// undefinedTable is PostgreSQL's SQLSTATE for a missing relation.
const undefinedTable = "42P01"

// isUndefinedTable reports whether err is PostgreSQL's missing-relation error.
func isUndefinedTable(err error) bool {
	var pgErr *pgconn.PgError
	return errors.As(err, &pgErr) && pgErr.Code == undefinedTable
}
