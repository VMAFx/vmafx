// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/queue/queue.go — job queue with in-memory FIFO and
// SQLite-backed persistence for crash recovery.
//
// The Queue interface is intentionally minimal for Phase 4b.1.  The backing
// store is modernc.org/sqlite (pure-Go, no cgo required for the controller
// binary outside of the scoring path).
//
// Lifecycle:
//   - New(dbPath, log) opens (or creates) the SQLite database, applies the
//     schema, and loads any PENDING jobs into the in-memory FIFO.
//   - Submit adds a job to both the database and the FIFO.
//   - PullWork atomically moves the oldest matching PENDING job to RUNNING
//     and assigns it to the given nodeID.
//   - ReportResult marks the job COMPLETED or FAILED and records results.
//   - Cancel marks the job CANCELLED (no-op if already terminal).
//   - Get returns a snapshot of any job by ID.
//   - Close shuts down the database connection.
//
// Thread safety: all exported methods are safe for concurrent use.
//
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
// ADR-0961: PullWork rolls back RUNNING state when post-update Get fails.

package queue

import (
	"context"
	"database/sql"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"sync"
	"time"

	"github.com/google/uuid"
	_ "modernc.org/sqlite" // pure-Go SQLite driver
)

//go:embed schema.sql
var schemaSQL string

// StatusPending etc. are the canonical status strings stored in SQLite.
const (
	StatusPending   = "pending"
	StatusRunning   = "running"
	StatusCompleted = "completed"
	StatusFailed    = "failed"
	StatusCancelled = "cancelled"
)

// ScoringParams mirrors the proto ScoringParams message without importing the
// generated proto code (which lives in a separate package).  The controller
// stores these as JSON in the jobs.scoring column.
type ScoringParams struct {
	Reference string `json:"reference"`
	Distorted string `json:"distorted"`
	Model     string `json:"model"`
	Backend   string `json:"backend"`
}

// Job is the in-memory representation of a queued work item.
type Job struct {
	ID           string
	Status       string
	Scoring      ScoringParams
	AssignedNode string
	Score        float64
	Features     map[string]float64
	Error        string
	// TenantID is the tenant that submitted this job (from the JWT "tid" claim).
	// All controller queries are scoped to this value (ADR-0794).
	TenantID  string
	CreatedAt time.Time
	UpdatedAt time.Time
}

// JobResult carries the terminal outcome reported by a vmafx-node.
type JobResult struct {
	Score    float64
	Features map[string]float64
	Err      string // non-empty → job failed
}

// NodeCapacity describes the requesting node's current capacity.  The scheduler
// uses this to filter matching jobs.
type NodeCapacity struct {
	// Backends is the list of available backend strings, e.g. ["cuda", "cpu"].
	Backends []string
	// Slots is the number of concurrent scoring jobs the node can accept.
	Slots int
}

// Queue is the interface implemented by *SQLiteQueue.
type Queue interface {
	// Submit enqueues a new job and returns its assigned ID.
	Submit(ctx context.Context, job *Job) (string, error)
	// PullWork atomically assigns the next matching PENDING job to nodeID.
	// Returns (nil, nil) when no matching job is available.
	PullWork(ctx context.Context, nodeID string, capacity NodeCapacity) (*Job, error)
	// ReportResult records the terminal outcome of a job.
	ReportResult(ctx context.Context, jobID string, result *JobResult) error
	// Get returns a snapshot of a job by ID.
	Get(ctx context.Context, jobID string) (*Job, error)
	// Cancel requests cancellation of a pending or running job.
	Cancel(ctx context.Context, jobID string) error
	// ListAll returns a snapshot of all jobs, optionally filtered to the
	// provided statuses.  An empty statuses slice returns all jobs.
	// Used by StreamJobs to deliver a consistent point-in-time snapshot.
	ListAll(ctx context.Context, statuses []string) ([]*Job, error)
	// PendingCount returns the current number of PENDING jobs.
	PendingCount() int
	// RunningCount returns the current number of RUNNING jobs.
	RunningCount() int
	// Close releases database resources.
	Close() error
}

// SQLiteQueue is the concrete implementation.
type SQLiteQueue struct {
	db  *sql.DB
	log *slog.Logger
	mu  sync.Mutex

	// pendingFIFO holds IDs of PENDING jobs in submission order.
	pendingFIFO []string
	// runningSet tracks IDs of RUNNING jobs for counter accuracy.
	runningSet map[string]struct{}

	// getUnlockedHook is called at the start of getUnlocked when non-nil.
	// It is used in tests to inject failures on demand; production code
	// always leaves this nil.  ADR-0961.
	getUnlockedHook func(id string) error
}

// New opens (or creates) the SQLite database at dbPath, applies the schema,
// and reconstructs the in-memory FIFO from any pre-existing PENDING rows.
//
// On any post-open failure (WAL pragma, schema apply, reload) the db handle
// is closed and any close-time error is joined onto the primary failure via
// errors.Join so callers can see both halves of the cleanup.
func New(dbPath string, log *slog.Logger) (*SQLiteQueue, error) {
	db, err := sql.Open("sqlite", dbPath)
	if err != nil {
		return nil, fmt.Errorf("queue: open sqlite %q: %w", dbPath, err)
	}

	// Enable WAL mode for better concurrent read performance.
	if _, err = db.Exec("PRAGMA journal_mode=WAL"); err != nil {
		return nil, closeAndJoin(db, fmt.Errorf("queue: enable WAL: %w", err))
	}

	// Apply schema (idempotent via CREATE TABLE IF NOT EXISTS).
	if _, err = db.Exec(schemaSQL); err != nil {
		return nil, closeAndJoin(db, fmt.Errorf("queue: apply schema: %w", err))
	}

	q := &SQLiteQueue{
		db:         db,
		log:        log,
		runningSet: make(map[string]struct{}),
	}

	// Reload in-flight state from the previous run.
	if err = q.reload(); err != nil {
		return nil, closeAndJoin(db, fmt.Errorf("queue: reload state: %w", err))
	}

	log.Info("job queue opened",
		"db", dbPath,
		"pending", len(q.pendingFIFO),
		"running_reset", len(q.runningSet),
	)
	return q, nil
}

// reload reconstructs in-memory state from the database after a restart.
// Running jobs are reset to PENDING (the assigned node is gone).
func (q *SQLiteQueue) reload() error {
	// Reset any RUNNING jobs to PENDING — the nodes that were executing them
	// are no longer connected after a controller restart.
	_, err := q.db.Exec(
		"UPDATE jobs SET status=?, assigned_node=NULL, updated_at=? WHERE status=?",
		StatusPending, time.Now().Unix(), StatusRunning,
	)
	if err != nil {
		return fmt.Errorf("reset running jobs: %w", err)
	}

	// Load PENDING jobs in submission order.
	rows, err := q.db.Query(
		"SELECT id FROM jobs WHERE status=? ORDER BY created_at ASC",
		StatusPending,
	)
	if err != nil {
		return fmt.Errorf("load pending jobs: %w", err)
	}
	defer func() {
		if closeErr := rows.Close(); closeErr != nil {
			q.log.Warn("queue: close reload rows", "error", closeErr)
		}
	}()

	for rows.Next() {
		var id string
		if err = rows.Scan(&id); err != nil {
			return fmt.Errorf("scan pending job id: %w", err)
		}
		q.pendingFIFO = append(q.pendingFIFO, id)
	}
	return rows.Err()
}

// Submit enqueues a new job.  The job's ID is assigned here (UUID v4) and
// written to both SQLite and the in-memory FIFO.
func (q *SQLiteQueue) Submit(ctx context.Context, job *Job) (string, error) {
	job.ID = uuid.New().String()
	job.Status = StatusPending
	now := time.Now()
	job.CreatedAt = now
	job.UpdatedAt = now

	scoringJSON, err := json.Marshal(job.Scoring)
	if err != nil {
		return "", fmt.Errorf("queue: marshal scoring params: %w", err)
	}

	// ExecContext propagates the caller's ctx so a cancelled gRPC SubmitJob
	// can abort the INSERT instead of holding the SQLite write open.
	_, err = q.db.ExecContext(ctx,
		"INSERT INTO jobs (id, status, scoring, tenant_id, created_at, updated_at) VALUES (?,?,?,?,?,?)",
		job.ID, StatusPending, string(scoringJSON), job.TenantID, now.Unix(), now.Unix(),
	)
	if err != nil {
		return "", fmt.Errorf("queue: insert job %s: %w", job.ID, err)
	}

	q.mu.Lock()
	q.pendingFIFO = append(q.pendingFIFO, job.ID)
	q.mu.Unlock()

	q.log.Info("job submitted", "job_id", job.ID, "tenant_id", job.TenantID, "reference", job.Scoring.Reference, "backend", job.Scoring.Backend)
	return job.ID, nil
}

// PullWork atomically dequeues the oldest PENDING job whose backend requirement
// (if any) is satisfied by the requesting node's capabilities.  Returns
// (nil, nil) when no matching job is available.
func (q *SQLiteQueue) PullWork(ctx context.Context, nodeID string, capacity NodeCapacity) (*Job, error) {
	q.mu.Lock()
	defer q.mu.Unlock()

	backendSet := make(map[string]struct{}, len(capacity.Backends))
	for _, b := range capacity.Backends {
		backendSet[b] = struct{}{}
	}

	// Find the first PENDING job whose backend requirement is satisfied.
	matchIdx := -1
	var matchID string
	for i, id := range q.pendingFIFO {
		job, err := q.getUnlocked(id)
		if err != nil {
			q.log.Warn("queue: failed to fetch pending job", "id", id, "error", err)
			continue
		}
		if job.Status != StatusPending {
			// Stale FIFO entry (job was cancelled externally) — drop it.
			continue
		}
		// Backend match: if the job specifies a backend, the node must support it.
		if job.Scoring.Backend == "" || len(backendSet) == 0 {
			matchIdx = i
			matchID = id
			break
		}
		if _, ok := backendSet[job.Scoring.Backend]; ok {
			matchIdx = i
			matchID = id
			break
		}
	}

	if matchIdx < 0 {
		return nil, nil
	}

	// Remove from FIFO.
	q.pendingFIFO = append(q.pendingFIFO[:matchIdx], q.pendingFIFO[matchIdx+1:]...)

	// Transition to RUNNING in SQLite.  The AND status=? guard prevents a
	// concurrent Cancel from being silently overwritten between the FIFO scan
	// above (under q.mu) and this write (r3-concurrency finding).
	// ExecContext propagates the caller's ctx so an aborted PullWork RPC
	// does not leave the UPDATE in flight.
	now := time.Now().Unix()
	res, err := q.db.ExecContext(ctx,
		"UPDATE jobs SET status=?, assigned_node=?, updated_at=? WHERE id=? AND status=?",
		StatusRunning, nodeID, now, matchID, StatusPending,
	)
	if err != nil {
		// Roll back in-memory assignment.
		q.pendingFIFO = append([]string{matchID}, q.pendingFIFO...)
		return nil, fmt.Errorf("queue: assign job %s: %w", matchID, err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		// A concurrent Cancel beat us: the job is no longer PENDING.
		// Re-prepend to FIFO so a retry loop can skip it.
		q.pendingFIFO = append([]string{matchID}, q.pendingFIFO...)
		return nil, fmt.Errorf("queue: job %s was cancelled before assignment; retry", matchID)
	}

	q.runningSet[matchID] = struct{}{}

	job, err := q.getUnlocked(matchID)
	if err != nil {
		// The SQL UPDATE already committed (status=running, assigned_node set)
		// and the FIFO entry was removed.  We must roll back all three changes
		// so the job is not permanently stranded in RUNNING state.
		// ADR-0961: PullWork rollback on post-update Get failure.
		rbErr := q.rollbackTopending(matchID)
		if rbErr != nil {
			q.log.Error("CRITICAL: rollback failed after PullWork Get error; job is stranded in RUNNING state — restart controller to recover",
				"job_id", matchID,
				"get_error", err,
				"rollback_error", rbErr,
			)
			return nil, fmt.Errorf("queue: fetch assigned job %s: %w; rollback also failed: %v", matchID, err, rbErr)
		}
		return nil, fmt.Errorf("queue: fetch assigned job %s: %w", matchID, err)
	}

	q.log.Info("job assigned", "job_id", matchID, "node_id", nodeID)
	return job, nil
}

// rollbackTopending reverses a PullWork that succeeded at the SQL UPDATE step
// but subsequently failed.  It must be called with q.mu held.
//
// Steps (ADR-0961):
//  1. Reset SQL status to PENDING and clear assigned_node.
//  2. Remove from runningSet.
//  3. Re-prepend the FIFO entry so the job is retried next.
func (q *SQLiteQueue) rollbackTopending(jobID string) error {
	_, err := q.db.Exec(
		"UPDATE jobs SET status=?, assigned_node=NULL, updated_at=? WHERE id=?",
		StatusPending, time.Now().Unix(), jobID,
	)
	if err != nil {
		return fmt.Errorf("queue: rollback SQL for job %s: %w", jobID, err)
	}
	delete(q.runningSet, jobID)
	q.pendingFIFO = append([]string{jobID}, q.pendingFIFO...)
	return nil
}

// ReportResult records the terminal outcome of a job.  If result.Err is
// non-empty the job is marked FAILED; otherwise COMPLETED.
func (q *SQLiteQueue) ReportResult(ctx context.Context, jobID string, result *JobResult) error {
	status := StatusCompleted
	if result.Err != "" {
		status = StatusFailed
	}

	featuresJSON, err := json.Marshal(result.Features)
	if err != nil {
		// map[string]float64 marshal can only fail on non-finite floats (NaN/Inf);
		// surface the error rather than silently discarding per-feature scores.
		return fmt.Errorf("queue: marshal features for job %s: %w", jobID, err)
	}

	// ExecContext propagates the caller's ctx so the node's ReportResult RPC
	// deadline / cancellation aborts the UPDATE cleanly.
	// The AND status NOT IN guard makes ReportResult idempotent: a node that
	// retries after a transient gRPC error will not overwrite an already-terminal
	// row, and a Cancel that races with ReportResult cannot be silently undone
	// (r4-retry-idempotency finding).
	now := time.Now().Unix()
	res, err := q.db.ExecContext(ctx,
		"UPDATE jobs SET status=?, score=?, features=?, error=?, updated_at=? WHERE id=? AND status NOT IN (?,?,?)",
		status, result.Score, string(featuresJSON), result.Err, now, jobID,
		StatusCompleted, StatusFailed, StatusCancelled,
	)
	if err != nil {
		return fmt.Errorf("queue: report result for job %s: %w", jobID, err)
	}
	if n, _ := res.RowsAffected(); n == 0 {
		// Already in a terminal state (completed, failed, or cancelled by the
		// time this call arrived) — treat as idempotent success.
		q.log.Info("ReportResult: job already in terminal state, ignoring", "job_id", jobID)
	}

	q.mu.Lock()
	delete(q.runningSet, jobID)
	q.mu.Unlock()

	q.log.Info("job result recorded", "job_id", jobID, "status", status, "score", result.Score)
	return nil
}

// Get returns a snapshot of a job by ID.
func (q *SQLiteQueue) Get(_ context.Context, jobID string) (*Job, error) {
	q.mu.Lock()
	defer q.mu.Unlock()
	return q.getUnlocked(jobID)
}

// getUnlocked fetches a job from SQLite without acquiring q.mu.
// Must be called with q.mu held (or from contexts that don't need the lock).
// If q.getUnlockedHook is non-nil it is invoked first; a non-nil error from
// the hook causes an early return (used in tests — ADR-0961).
func (q *SQLiteQueue) getUnlocked(jobID string) (*Job, error) {
	if q.getUnlockedHook != nil {
		if err := q.getUnlockedHook(jobID); err != nil {
			return nil, err
		}
	}
	row := q.db.QueryRow(
		"SELECT id, status, scoring, COALESCE(assigned_node,''), COALESCE(score,0), COALESCE(features,'{}'), COALESCE(error,''), COALESCE(tenant_id,''), created_at, updated_at FROM jobs WHERE id=?",
		jobID,
	)

	var (
		job          Job
		scoringJSON  string
		featuresJSON string
		createdAt    int64
		updatedAt    int64
	)
	err := row.Scan(
		&job.ID, &job.Status, &scoringJSON, &job.AssignedNode,
		&job.Score, &featuresJSON, &job.Error, &job.TenantID,
		&createdAt, &updatedAt,
	)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return nil, fmt.Errorf("queue: job %s not found", jobID)
		}
		return nil, fmt.Errorf("queue: scan job %s: %w", jobID, err)
	}

	if err = json.Unmarshal([]byte(scoringJSON), &job.Scoring); err != nil {
		return nil, fmt.Errorf("queue: unmarshal scoring for job %s: %w", jobID, err)
	}
	if err = json.Unmarshal([]byte(featuresJSON), &job.Features); err != nil {
		job.Features = map[string]float64{}
	}
	job.CreatedAt = time.Unix(createdAt, 0)
	job.UpdatedAt = time.Unix(updatedAt, 0)
	return &job, nil
}

// Cancel marks a PENDING or RUNNING job as CANCELLED.  Returns nil if the job
// was already in a terminal state (idempotent).
func (q *SQLiteQueue) Cancel(ctx context.Context, jobID string) error {
	// ExecContext propagates the caller's ctx so an aborted CancelJob RPC
	// does not leave the UPDATE in flight.
	now := time.Now().Unix()
	res, err := q.db.ExecContext(ctx,
		"UPDATE jobs SET status=?, updated_at=? WHERE id=? AND status IN (?,?)",
		StatusCancelled, now, jobID, StatusPending, StatusRunning,
	)
	if err != nil {
		return fmt.Errorf("queue: cancel job %s: %w", jobID, err)
	}

	rows, _ := res.RowsAffected()
	if rows == 0 {
		// Job was already terminal — treat as idempotent success.
		q.log.Debug("cancel no-op (already terminal)", "job_id", jobID)
		return nil
	}

	// Remove from in-memory structures.
	q.mu.Lock()
	defer q.mu.Unlock()
	for i, id := range q.pendingFIFO {
		if id == jobID {
			q.pendingFIFO = append(q.pendingFIFO[:i], q.pendingFIFO[i+1:]...)
			break
		}
	}
	delete(q.runningSet, jobID)

	q.log.Info("job cancelled", "job_id", jobID)
	return nil
}

// PendingCount returns the current number of PENDING jobs (from in-memory FIFO).
func (q *SQLiteQueue) PendingCount() int {
	q.mu.Lock()
	defer q.mu.Unlock()
	return len(q.pendingFIFO)
}

// RunningCount returns the current number of RUNNING jobs.
func (q *SQLiteQueue) RunningCount() int {
	q.mu.Lock()
	defer q.mu.Unlock()
	return len(q.runningSet)
}

// Depth satisfies observability.QueueDepthProvider (ADR-0782).
// Returns the current number of PENDING jobs — equivalent to PendingCount.
func (q *SQLiteQueue) Depth() int { return q.PendingCount() }

// SetGetUnlockedHookForTest installs a function that is called at the start of
// every getUnlocked call.  When the hook returns a non-nil error, getUnlocked
// returns that error immediately without hitting SQLite.
//
// This is a test-only escape hatch; do not call it in production code.
// ADR-0961: hook exists solely to exercise the PullWork rollback path.
func (q *SQLiteQueue) SetGetUnlockedHookForTest(fn func(id string) error) {
	q.mu.Lock()
	defer q.mu.Unlock()
	q.getUnlockedHook = fn
}

// ListAll returns a point-in-time snapshot of all jobs, optionally filtered
// to the provided statuses.  An empty statuses slice returns every job.
//
// Callers receive copies — mutations to the returned slice do not affect the
// queue.  Used by controllerServer.StreamJobs to send a consistent snapshot
// (ADR-0962).
func (q *SQLiteQueue) ListAll(_ context.Context, statuses []string) ([]*Job, error) {
	var (
		rows *sql.Rows
		err  error
	)

	if len(statuses) == 0 {
		rows, err = q.db.Query(
			"SELECT id, status, scoring, COALESCE(assigned_node,''), COALESCE(score,0), COALESCE(features,'{}'), COALESCE(error,''), COALESCE(tenant_id,''), created_at, updated_at FROM jobs ORDER BY created_at ASC",
		)
	} else {
		// Build a parameterised IN clause.  We limit statuses to the known set
		// (max 5) so the query never becomes unbounded.
		placeholders := make([]any, len(statuses))
		for i, s := range statuses {
			placeholders[i] = s
		}
		// #nosec G202 -- The concatenated fragment is repeatCommaQ output, a
		// pure ",?,?,..." placeholder string of length len(statuses)-1; no
		// user data enters the SQL text. Status values bind through
		// `placeholders...` as parameterised arguments.
		query := "SELECT id, status, scoring, COALESCE(assigned_node,''), COALESCE(score,0), COALESCE(features,'{}'), COALESCE(error,''), COALESCE(tenant_id,''), created_at, updated_at FROM jobs WHERE status IN (?" + repeatCommaQ(len(statuses)-1) + ") ORDER BY created_at ASC"
		rows, err = q.db.Query(query, placeholders...)
	}
	if err != nil {
		return nil, fmt.Errorf("queue: list all jobs: %w", err)
	}
	defer func() {
		if closeErr := rows.Close(); closeErr != nil {
			q.log.Warn("queue: close ListAll rows", "error", closeErr)
		}
	}()

	var out []*Job
	for rows.Next() {
		var (
			job          Job
			scoringJSON  string
			featuresJSON string
			createdAt    int64
			updatedAt    int64
		)
		if err = rows.Scan(
			&job.ID, &job.Status, &scoringJSON, &job.AssignedNode,
			&job.Score, &featuresJSON, &job.Error, &job.TenantID,
			&createdAt, &updatedAt,
		); err != nil {
			return nil, fmt.Errorf("queue: scan job row in ListAll: %w", err)
		}
		if err = json.Unmarshal([]byte(scoringJSON), &job.Scoring); err != nil {
			return nil, fmt.Errorf("queue: unmarshal scoring in ListAll: %w", err)
		}
		if err = json.Unmarshal([]byte(featuresJSON), &job.Features); err != nil {
			job.Features = map[string]float64{}
		}
		job.CreatedAt = time.Unix(createdAt, 0)
		job.UpdatedAt = time.Unix(updatedAt, 0)
		cp := job
		out = append(out, &cp)
	}
	if err = rows.Err(); err != nil {
		return nil, fmt.Errorf("queue: iterate jobs in ListAll: %w", err)
	}
	return out, nil
}

// repeatCommaQ returns n comma-prefixed "?" placeholders (e.g. n=2 → ",?,?").
func repeatCommaQ(n int) string {
	if n <= 0 {
		return ""
	}
	buf := make([]byte, n*2)
	for i := range n {
		buf[i*2] = ','
		buf[i*2+1] = '?'
	}
	return string(buf)
}

// Close releases the database connection.
func (q *SQLiteQueue) Close() error {
	return q.db.Close()
}

// closeAndJoin closes db and joins any close-time error onto the primary
// failure via errors.Join. Used by New() to ensure the db handle is not
// leaked when a post-open initialisation step fails while still surfacing
// both the primary error and any close-time error to the caller.
func closeAndJoin(db *sql.DB, primary error) error {
	closeErr := db.Close()
	if closeErr == nil {
		return primary
	}
	return errors.Join(primary, fmt.Errorf("queue: close db after init failure: %w", closeErr))
}
