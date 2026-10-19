// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/backend/command.go — the controller's one-shot
// commands for the PostgreSQL backend: migrate and import-sqlite.

package backend

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/golusoris/golusoris/jobs"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store"
)

// DSNEnv names the environment variable of the PostgreSQL connection string
// (golusoris db/pgx reads the same key as db.dsn).
const DSNEnv = "VMAFX_DB_DSN"

// Bounds of the one-shot commands (HISS-02).
const (
	migrateTimeout = 10 * time.Minute
	importTimeout  = 2 * time.Hour
)

// Exit codes of the one-shot commands.
const (
	ExitOK    = 0
	ExitError = 1
	ExitUsage = 2
)

// Command runs a one-shot command when args (os.Args) name one, and reports
// whether they did and the exit code. getenv reads the environment.
func Command(args []string, getenv func(string) string, out io.Writer, logger *slog.Logger) (bool, int) {
	if len(args) < 2 {
		return false, ExitOK
	}
	switch args[1] {
	case "migrate":
		return true, report(out, runMigrate(getenv, logger))
	case "import-sqlite":
		return true, runImportCommand(args[2:], getenv, out)
	default:
		return false, ExitOK
	}
}

// report prints err and maps it to an exit code.
func report(out io.Writer, err error) int {
	if err == nil {
		return ExitOK
	}
	if werr := printf(out, "vmafx-controller: %v\n", err); werr != nil || !errors.Is(err, errUsage) {
		return ExitError
	}
	return ExitUsage
}

// printf writes to the command's output; a failed write fails the command.
func printf(out io.Writer, format string, a ...any) error {
	if _, err := fmt.Fprintf(out, format, a...); err != nil {
		return fmt.Errorf("write output: %w", err)
	}
	return nil
}

// errUsage marks a command line the command cannot run.
var errUsage = errors.New("usage")

// openPool connects to the database the environment names.
func openPool(ctx context.Context, getenv func(string) string) (*pgxpool.Pool, error) {
	dsn := getenv(DSNEnv)
	if dsn == "" {
		return nil, fmt.Errorf("%w: %s is not set", errUsage, DSNEnv)
	}
	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		return nil, fmt.Errorf("connect to the database: %w", err)
	}
	if err := pool.Ping(ctx); err != nil {
		pool.Close()
		return nil, fmt.Errorf("connect to the database: %w", err)
	}
	return pool, nil
}

// runMigrate applies the store's and River's migrations.
func runMigrate(getenv func(string) string, logger *slog.Logger) error {
	ctx, cancel := context.WithTimeout(context.Background(), migrateTimeout)
	defer cancel()
	pool, err := openPool(ctx, getenv)
	if err != nil {
		return err
	}
	defer pool.Close()
	if err := store.MigratePostgres(getenv(DSNEnv), logger); err != nil {
		return err
	}
	if err := jobs.Migrate(ctx, pool); err != nil {
		return fmt.Errorf("migrate River: %w", err)
	}
	return store.NewPostgres(pool).CheckSchema(ctx)
}

// runImportCommand parses the import-sqlite command line and runs it.
func runImportCommand(args []string, getenv func(string) string, out io.Writer) int {
	fs := flag.NewFlagSet("import-sqlite", flag.ContinueOnError)
	fs.SetOutput(out)
	from := fs.String("from", "", "SQLite queue file to read (VMAFX_DB_PATH of the old controller)")
	emptyTenant := fs.String("empty-tenant", "", "tenant to give jobs that carry none")
	if err := fs.Parse(args); err != nil {
		return ExitUsage
	}
	if *from == "" || fs.NArg() != 0 {
		if err := printf(out, "usage: vmafx-controller import-sqlite --from <file> [--empty-tenant <tenant>]\n"); err != nil {
			return ExitError
		}
		return ExitUsage
	}
	rep, err := runImport(*from, ImportOptions{EmptyTenant: *emptyTenant}, getenv)
	if code := report(out, err); code != ExitOK {
		return code
	}
	if err := printf(out, "imported %d jobs (%d back in the queue), %d already present, %d with a malformed ID left out\n",
		rep.Imported, rep.Requeued, rep.Present, rep.Malformed); err != nil {
		return ExitError
	}
	return ExitOK
}

// runImport copies the queue file into the migrated store.
func runImport(from string, opts ImportOptions, getenv func(string) string) (ImportReport, error) {
	ctx, cancel := context.WithTimeout(context.Background(), importTimeout)
	defer cancel()
	pool, err := openPool(ctx, getenv)
	if err != nil {
		return ImportReport{}, err
	}
	defer pool.Close()
	st := store.NewPostgres(pool)
	if err := st.CheckSchema(ctx); err != nil {
		return ImportReport{}, fmt.Errorf("%w (run vmafx-controller migrate first)", err)
	}
	return ImportSQLite(ctx, from, st, opts)
}
