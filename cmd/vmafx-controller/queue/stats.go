// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package queue

import (
	"context"
	"database/sql"
	"fmt"
	"maps"
	"time"
)

// Requeue reasons: why a RUNNING job went back to PENDING. They are the values
// of the reason label of vmafx_controller_jobs_requeued_total
// (pkg/observability/metricdef.RequeueReason).
const (
	// RequeueNodeLost is a job of a node the registry evicted (RequeueNode).
	RequeueNodeLost = "node_lost"
	// RequeueRestart is a job that was running when the controller stopped;
	// New returns it to the queue.
	RequeueRestart = "controller_restart"
	// RequeueRollback is a job whose assignment failed after the queue had
	// moved it to RUNNING (ADR-0961).
	RequeueRollback = "assign_rollback"
)

// TenantStats are the live counts of one tenant's jobs.
type TenantStats struct {
	TenantID string
	Pending  int
	Running  int
	// OldestPending is the submission time of the tenant's oldest PENDING
	// job; zero when the tenant has none.
	OldestPending time.Time
}

// Stats is what the controller's /metrics page reads from the queue: the
// per-tenant counts of PENDING and RUNNING jobs, and the requeue totals by
// reason since the queue opened.
type Stats struct {
	Tenants  []TenantStats
	Requeued map[string]uint64
}

// statsQuery counts the PENDING and RUNNING jobs of every tenant and finds the
// oldest PENDING one, using idx_jobs_tenant_status. It reads counts and times,
// never a job's content.
const statsQuery = "SELECT tenant_id, status, COUNT(*), MIN(created_at) FROM jobs " +
	"WHERE status IN (?, ?) GROUP BY tenant_id, status ORDER BY tenant_id, status"

// Stats returns the queue's counts; ctx bounds the read.
func (q *SQLiteQueue) Stats(ctx context.Context) (Stats, error) {
	rows, err := q.db.QueryContext(ctx, statsQuery, StatusPending, StatusRunning)
	if err != nil {
		return Stats{}, fmt.Errorf("queue: read stats: %w", err)
	}
	tenants, err := scanStats(rows)
	if closeErr := rows.Close(); closeErr != nil && err == nil {
		err = fmt.Errorf("queue: close stats rows: %w", closeErr)
	}
	if err != nil {
		return Stats{}, err
	}
	q.mu.Lock()
	requeued := maps.Clone(q.requeued)
	q.mu.Unlock()
	return Stats{Tenants: tenants, Requeued: requeued}, nil
}

// scanStats folds the (tenant, status) rows of statsQuery into one
// TenantStats per tenant, in tenant order.
func scanStats(rows *sql.Rows) ([]TenantStats, error) {
	var out []TenantStats
	for rows.Next() {
		var (
			tenant, status string
			count          int
			oldest         int64
		)
		if err := rows.Scan(&tenant, &status, &count, &oldest); err != nil {
			return nil, fmt.Errorf("queue: scan stats: %w", err)
		}
		if len(out) == 0 || out[len(out)-1].TenantID != tenant {
			out = append(out, TenantStats{TenantID: tenant})
		}
		ts := &out[len(out)-1]
		if status == StatusPending {
			ts.Pending = count
			ts.OldestPending = time.Unix(oldest, 0)
		} else {
			ts.Running = count
		}
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("queue: iterate stats: %w", err)
	}
	return out, nil
}
