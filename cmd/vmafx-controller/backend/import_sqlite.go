// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/backend/import_sqlite.go — carry the jobs of the
// embedded SQLite queue into the PostgreSQL store (ADR-2350 D16).

package backend

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"path/filepath"
	"time"

	"github.com/google/uuid"
	_ "modernc.org/sqlite" // the driver of the queue file being read

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store"
)

// MaxImportJobs bounds one import (HISS-02); a larger queue file is refused.
const MaxImportJobs = 10_000_000

// ErrNotQueueFile is a file that is not a database of the SQLite queue.
var ErrNotQueueFile = errors.New("backend: not a job queue database of vmafx-controller")

// ImportOptions shapes an import.
type ImportOptions struct {
	// EmptyTenant is the tenant given to jobs that carry none (a queue run
	// with authentication disabled). Empty: such jobs are refused.
	EmptyTenant string
}

// ImportReport counts what an import did.
type ImportReport struct {
	Imported  int
	Present   int // already in the store (an earlier run)
	Requeued  int // pending or running jobs, imported as pending
	Malformed int // rows with an ID that is not a UUID; left out
}

// legacyJobsQuery reads every job of the queue's schema
// (cmd/vmafx-controller/queue/schema.sql).
const legacyJobsQuery = `SELECT id, status, scoring, COALESCE(assigned_node, ''), score,
	COALESCE(features, ''), COALESCE(error, ''), COALESCE(tenant_id, ''), created_at, updated_at
	FROM jobs ORDER BY created_at, id`

// ImportSQLite copies the jobs of the SQLite queue file at path into st,
// read-only on the file. A second run skips the jobs the first one copied.
func ImportSQLite(ctx context.Context, path string, st *store.Postgres, opts ImportOptions) (rep ImportReport, err error) {
	db, err := openQueueFile(path)
	if err != nil {
		return ImportReport{}, err
	}
	defer func() {
		if cerr := db.Close(); cerr != nil {
			err = errors.Join(err, fmt.Errorf("backend: close %s: %w", path, cerr))
		}
	}()
	rows, err := db.QueryContext(ctx, legacyJobsQuery)
	if err != nil {
		return ImportReport{}, fmt.Errorf("%w: %s: %w", ErrNotQueueFile, path, err)
	}
	defer func() {
		if cerr := rows.Close(); cerr != nil {
			err = errors.Join(err, fmt.Errorf("backend: close rows of %s: %w", path, cerr))
		}
	}()
	for n := 0; rows.Next(); n++ {
		if n >= MaxImportJobs {
			return rep, fmt.Errorf("backend: %s holds more than %d jobs", path, MaxImportJobs)
		}
		job, ok, err := scanLegacyJob(rows, opts)
		if err != nil {
			return rep, err
		}
		if !ok {
			rep.Malformed++
			continue
		}
		if err := importOne(ctx, st, job, &rep); err != nil {
			return rep, err
		}
	}
	if err := rows.Err(); err != nil {
		return rep, fmt.Errorf("backend: read %s: %w", path, err)
	}
	return rep, nil
}

// openQueueFile opens the queue file read-only and checks it is one.
func openQueueFile(path string) (*sql.DB, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, fmt.Errorf("backend: %s: %w", path, err)
	}
	dsn := (&url.URL{Scheme: "file", Path: filepath.ToSlash(abs), RawQuery: "mode=ro"}).String()
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("backend: open %s: %w", path, err)
	}
	var columns int
	if err := db.QueryRow(`SELECT count(*) FROM pragma_table_info('jobs')
		WHERE name IN ('id', 'status', 'scoring', 'tenant_id', 'created_at')`).Scan(&columns); err != nil || columns != 5 {
		return nil, errors.Join(fmt.Errorf("%w: %s", ErrNotQueueFile, path), db.Close())
	}
	return db, nil
}

// scanLegacyJob reads one row; ok is false for a row whose ID is not a UUID.
func scanLegacyJob(rows *sql.Rows, opts ImportOptions) (store.ImportedJob, bool, error) {
	var (
		id, status, scoring, node, features, errText, tenant string
		score                                                sql.NullFloat64
		created, updated                                     int64
	)
	if err := rows.Scan(&id, &status, &scoring, &node, &score, &features, &errText, &tenant, &created, &updated); err != nil {
		return store.ImportedJob{}, false, fmt.Errorf("backend: read queue row: %w", err)
	}
	jobID, err := uuid.Parse(id)
	if err != nil {
		return store.ImportedJob{}, false, nil
	}
	if tenant == "" {
		if opts.EmptyTenant == "" {
			return store.ImportedJob{}, false, fmt.Errorf("backend: job %s has no tenant; pass the tenant to give it", id)
		}
		tenant = opts.EmptyTenant
	}
	job := store.ImportedJob{
		ID: jobID, TenantID: tenant, Status: store.Status(status), Spec: json.RawMessage(scoring),
		AssignedNode: node, Error: errText, CreatedAt: time.Unix(created, 0), UpdatedAt: time.Unix(updated, 0),
	}
	if score.Valid {
		job.Score = &score.Float64
	}
	if err := decodeLegacy(&job, features); err != nil {
		return store.ImportedJob{}, false, err
	}
	return job, true, nil
}

// decodeLegacy fills the backend and features of job from its JSON columns.
func decodeLegacy(job *store.ImportedJob, features string) error {
	var s Scoring
	if err := json.Unmarshal(job.Spec, &s); err != nil {
		return fmt.Errorf("backend: job %s: scoring: %w", job.ID, err)
	}
	job.Backend = s.Backend
	if features != "" && features != "null" {
		if err := json.Unmarshal([]byte(features), &job.Features); err != nil {
			return fmt.Errorf("backend: job %s: features: %w", job.ID, err)
		}
	}
	return nil
}

// importOne writes one job and counts it.
func importOne(ctx context.Context, st *store.Postgres, job store.ImportedJob, rep *ImportReport) error {
	written, err := st.Import(ctx, job)
	if err != nil {
		return err
	}
	if !written {
		rep.Present++
		return nil
	}
	rep.Imported++
	if job.Status == store.StatusPending || job.Status == store.StatusRunning {
		rep.Requeued++
	}
	return nil
}
