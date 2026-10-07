// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/backend/import_sqlite_test.go — jobs of a SQLite queue
// file carried into the PostgreSQL store.

package backend_test

import (
	"context"
	"database/sql"
	"errors"
	"log/slog"
	"os"
	"path/filepath"
	"testing"

	"github.com/google/uuid"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/backend"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
)

// legacyRow is one row of the SQLite queue's jobs table.
type legacyRow struct {
	id, status, tenant, node, features, errText string
	score                                       any
}

// queueFile creates a SQLite queue file with the queue's own schema
// (queue.New) and the given rows.
func queueFile(t *testing.T, rows []legacyRow) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "vmafx-controller.db")
	q, err := queue.New(path, slog.New(slog.DiscardHandler))
	if err != nil {
		t.Fatalf("queue.New: %v", err)
	}
	if err := q.Close(); err != nil {
		t.Fatalf("close queue: %v", err)
	}
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer func() { _ = db.Close() }()
	for i, r := range rows {
		_, err := db.Exec(`INSERT INTO jobs (id, status, scoring, assigned_node, score, features, error,
			tenant_id, created_at, updated_at) VALUES (?, ?, ?, NULLIF(?, ''), ?, NULLIF(?, ''), NULLIF(?, ''), ?, ?, ?)`,
			r.id, r.status, `{"reference":"r","distorted":"d","model":"m","backend":"cuda"}`,
			r.node, r.score, r.features, r.errText, r.tenant, 1_700_000_000+i, 1_700_000_100+i)
		if err != nil {
			t.Fatalf("insert %s: %v", r.id, err)
		}
	}
	return path
}

func TestImportSQLiteCarriesEveryJob(t *testing.T) {
	t.Parallel()
	b, db := newBackend(t)
	ctx := context.Background()
	pending, running, done := uuid.NewString(), uuid.NewString(), uuid.NewString()
	path := queueFile(t, []legacyRow{
		{id: pending, status: "pending", tenant: "t1"},
		{id: running, status: "running", tenant: "t1", node: "old-node"},
		{id: done, status: "completed", tenant: "t2", node: "n9", features: `{"vif":0.5}`, score: 77.5},
		{id: "not-a-uuid", status: "pending", tenant: "t1"},
	})
	rep, err := backend.ImportSQLite(ctx, path, db.Store, backend.ImportOptions{})
	if err != nil {
		t.Fatalf("import: %v", err)
	}
	if rep != (backend.ImportReport{Imported: 3, Requeued: 2, Malformed: 1}) {
		t.Fatalf("report: %+v", rep)
	}
	for _, id := range []string{pending, running} {
		if j, err := b.Get(ctx, "t1", id); err != nil || j.Status != backend.StatusPending || j.Scoring.Backend != "cuda" {
			t.Fatalf("job %s: %+v %v", id, j, err)
		}
	}
	j, err := b.Get(ctx, "t2", done)
	if err != nil || j.Status != backend.StatusCompleted || j.Score != 77.5 || j.Features["vif"] != 0.5 || j.AssignedNode != "n9" {
		t.Fatalf("finished job: %+v %v", j, err)
	}
	again, err := backend.ImportSQLite(ctx, path, db.Store, backend.ImportOptions{})
	if err != nil || again.Imported != 0 || again.Present != 3 {
		t.Fatalf("second import must skip what the first copied: %+v %v", again, err)
	}
}

func TestImportSQLiteNeedsATenantForTenantlessJobs(t *testing.T) {
	t.Parallel()
	b, db := newBackend(t)
	ctx := context.Background()
	id := uuid.NewString()
	path := queueFile(t, []legacyRow{{id: id, status: "pending"}})
	if _, err := backend.ImportSQLite(ctx, path, db.Store, backend.ImportOptions{}); err == nil {
		t.Fatalf("a job with no tenant was imported without one being named")
	}
	if _, err := backend.ImportSQLite(ctx, path, db.Store, backend.ImportOptions{EmptyTenant: "default"}); err != nil {
		t.Fatalf("import with a tenant for tenantless jobs: %v", err)
	}
	if _, err := b.Get(ctx, "default", id); err != nil {
		t.Fatalf("job under the named tenant: %v", err)
	}
}

func TestImportSQLiteRefusesOtherFiles(t *testing.T) {
	t.Parallel()
	_, db := newBackend(t)
	other := filepath.Join(t.TempDir(), "other.db")
	sdb, err := sql.Open("sqlite", other)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	if _, err := sdb.Exec("CREATE TABLE notes (id TEXT)"); err != nil {
		t.Fatalf("create: %v", err)
	}
	_ = sdb.Close()
	if _, err := backend.ImportSQLite(context.Background(), other, db.Store, backend.ImportOptions{}); !errors.Is(err, backend.ErrNotQueueFile) {
		t.Fatalf("another database: %v", err)
	}
	text := filepath.Join(t.TempDir(), "notes.txt")
	if err := os.WriteFile(text, []byte("not a database"), 0o600); err != nil {
		t.Fatalf("write: %v", err)
	}
	if _, err := backend.ImportSQLite(context.Background(), text, db.Store, backend.ImportOptions{}); !errors.Is(err, backend.ErrNotQueueFile) {
		t.Fatalf("a text file: %v", err)
	}
}
