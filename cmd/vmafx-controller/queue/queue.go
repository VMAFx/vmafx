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
	CreatedAt    time.Time
	UpdatedAt    time.Time
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
}

// New opens (or creates) the SQLite database at dbPath, applies the schema,
// and reconstructs the in-memory FIFO from any pre-existing PENDING rows.
func New(dbPath string, log *slog.Logger) (*SQLiteQueue, error) {
	db, err := sql.Open("sqlite", dbPath)
	if err != nil {
		return nil, fmt.Errorf("queue: open sqlite %q: %w", dbPath, err)
	}

	// Enable WAL mode for better concurrent read performance.
	if _, err = db.Exec("PRAGMA journal_mode=WAL"); err != nil {
		db.Close() //nolint:errcheck // best-effort on error path
		return nil, fmt.Errorf("queue: enable WAL: %w", err)
	}

	// Apply schema (idempotent via CREATE TABLE IF NOT EXISTS).
	if _, err = db.Exec(schemaSQL); err != nil {
		db.Close() //nolint:errcheck // best-effort on error path
		return nil, fmt.Errorf("queue: apply schema: %w", err)
	}

	q := &SQLiteQueue{
		db:         db,
		log:        log,
		runningSet: make(map[string]struct{}),
	}

	// Reload in-flight state from the previous run.
	if err = q.reload(); err != nil {
		db.Close() //nolint:errcheck // best-effort on error path
		return nil, fmt.Errorf("queue: reload state: %w", err)
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
	defer rows.Close()

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
func (q *SQLiteQueue) Submit(_ context.Context, job *Job) (string, error) {
	job.ID = uuid.New().String()
	job.Status = StatusPending
	now := time.Now()
	job.CreatedAt = now
	job.UpdatedAt = now

	scoringJSON, err := json.Marshal(job.Scoring)
	if err != nil {
		return "", fmt.Errorf("queue: marshal scoring params: %w", err)
	}

	_, err = q.db.Exec(
		"INSERT INTO jobs (id, status, scoring, created_at, updated_at) VALUES (?,?,?,?,?)",
		job.ID, StatusPending, string(scoringJSON), now.Unix(), now.Unix(),
	)
	if err != nil {
		return "", fmt.Errorf("queue: insert job %s: %w", job.ID, err)
	}

	q.mu.Lock()
	q.pendingFIFO = append(q.pendingFIFO, job.ID)
	q.mu.Unlock()

	q.log.Info("job submitted", "job_id", job.ID, "reference", job.Scoring.Reference, "backend", job.Scoring.Backend)
	return job.ID, nil
}

// PullWork atomically dequeues the oldest PENDING job whose backend requirement
// (if any) is satisfied by the requesting node's capabilities.  Returns
// (nil, nil) when no matching job is available.
func (q *SQLiteQueue) PullWork(_ context.Context, nodeID string, capacity NodeCapacity) (*Job, error) {
	q.mu.Lock()
	defer q.mu.Unlock()

	backendSet := make(map[string]struct{}, len(capacity.Backends))
	for _, b := range capacity.Backends {
		backendSet[b] = struct{}{}
	}

	// Find the first PENDING job whose backend requirement is satisfied.
	var matchIdx int = -1
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

	// Transition to RUNNING in SQLite.
	now := time.Now().Unix()
	_, err := q.db.Exec(
		"UPDATE jobs SET status=?, assigned_node=?, updated_at=? WHERE id=?",
		StatusRunning, nodeID, now, matchID,
	)
	if err != nil {
		// Roll back in-memory assignment.
		q.pendingFIFO = append([]string{matchID}, q.pendingFIFO...)
		return nil, fmt.Errorf("queue: assign job %s: %w", matchID, err)
	}

	q.runningSet[matchID] = struct{}{}

	job, err := q.getUnlocked(matchID)
	if err != nil {
		return nil, fmt.Errorf("queue: fetch assigned job %s: %w", matchID, err)
	}

	q.log.Info("job assigned", "job_id", matchID, "node_id", nodeID)
	return job, nil
}

// ReportResult records the terminal outcome of a job.  If result.Err is
// non-empty the job is marked FAILED; otherwise COMPLETED.
func (q *SQLiteQueue) ReportResult(_ context.Context, jobID string, result *JobResult) error {
	status := StatusCompleted
	if result.Err != "" {
		status = StatusFailed
	}

	featuresJSON, err := json.Marshal(result.Features)
	if err != nil {
		featuresJSON = []byte("{}")
	}

	now := time.Now().Unix()
	_, err = q.db.Exec(
		"UPDATE jobs SET status=?, score=?, features=?, error=?, updated_at=? WHERE id=?",
		status, result.Score, string(featuresJSON), result.Err, now, jobID,
	)
	if err != nil {
		return fmt.Errorf("queue: report result for job %s: %w", jobID, err)
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
func (q *SQLiteQueue) getUnlocked(jobID string) (*Job, error) {
	row := q.db.QueryRow(
		"SELECT id, status, scoring, COALESCE(assigned_node,''), COALESCE(score,0), COALESCE(features,'{}'), COALESCE(error,''), created_at, updated_at FROM jobs WHERE id=?",
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
		&job.Score, &featuresJSON, &job.Error,
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
func (q *SQLiteQueue) Cancel(_ context.Context, jobID string) error {
	now := time.Now().Unix()
	res, err := q.db.Exec(
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

// Close releases the database connection.
func (q *SQLiteQueue) Close() error {
	return q.db.Close()
}
