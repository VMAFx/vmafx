// SPDX-License-Identifier: EUPL-1.2
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
//   - PullWork atomically moves the oldest matching PENDING job of the given
//     tenant to RUNNING and assigns it to the given nodeID.
//   - ReportResult marks the job COMPLETED or FAILED and records results,
//     provided the job is assigned to the reporting node (or to an orphaned
//     node of the reporter's tenant).
//   - Cancel marks the job CANCELLED (no-op if already terminal).
//   - Get returns a snapshot of any job by ID.
//   - Close shuts down the database connection.
//
// Thread safety: all exported methods are safe for concurrent use.
//
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
// ADR-0961: PullWork rolls back RUNNING state when post-update Get fails.
// ADR-1522: every read that returns jobs is scoped to one tenant; there is no
// read of all tenants' jobs.

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

// ErrNotAssigned is returned (wrapped) by ReportResult when the job does not
// exist, belongs to another tenant, or is not assigned to the reporting node
// (or to an orphaned node of its tenant).
var ErrNotAssigned = errors.New("job is not assigned to this node")

// Queue is the interface implemented by *SQLiteQueue.
type Queue interface {
	// Submit enqueues a new job and returns its assigned ID.
	Submit(ctx context.Context, job *Job) (string, error)
	// PullWork atomically assigns the next matching PENDING job of tenantID to
	// nodeID. Jobs of other tenants are never assigned. Returns (nil, nil)
	// when no matching job is available.
	PullWork(ctx context.Context, nodeID, tenantID string, capacity NodeCapacity) (*Job, error)
	// ReportResult records the terminal outcome of a job assigned to the
	// reporting node (or of an orphaned job of its tenant, see Report) and
	// reports whether this call moved the job to its terminal state; a
	// repeated report of a finished job reports false. It returns an error
	// wrapping ErrNotAssigned, and changes nothing, for any other job.
	ReportResult(ctx context.Context, r Report) (bool, error)
	// MayReport reports whether a partial report would be accepted; it writes
	// nothing.
	MayReport(ctx context.Context, r Report) bool
	// Get returns a snapshot of a job by ID.
	Get(ctx context.Context, jobID string) (*Job, error)
	// Cancel requests cancellation of a pending or running job. It reports
	// whether this call moved the job to CANCELLED; a job already in a
	// terminal state is left alone and reports false.
	Cancel(ctx context.Context, jobID string) (bool, error)
	// CancelledAmong returns the IDs of ids that name a CANCELLED job of
	// tenantID, in the order of ids. Unknown IDs and other tenants' jobs are
	// left out. The controller answers a node's Heartbeat with it, so the node
	// stops the running jobs a CancelJob reached (ADR-1567).
	CancelledAmong(ctx context.Context, tenantID string, ids []string) ([]string, error)
	// RequeueNode returns every RUNNING job assigned to nodeID to PENDING
	// (ahead of newer pending jobs) and reports how many it moved. The
	// controller calls it when the node registry evicts a silent node.
	RequeueNode(ctx context.Context, nodeID string) (int, error)
	// ListByTenant returns a snapshot of the jobs of tenantID, optionally
	// filtered to the provided statuses.  An empty statuses slice returns all
	// of the tenant's jobs.  Used by StreamJobs to deliver a consistent
	// point-in-time snapshot (ADR-0962, ADR-1522).
	ListByTenant(ctx context.Context, tenantID string, statuses []string) ([]*Job, error)
	// PendingCount returns the current number of PENDING jobs.
	PendingCount() int
	// RunningCount returns the current number of RUNNING jobs.
	RunningCount() int
	// Stats returns the queue's per-tenant counts and its requeue totals for
	// the controller's /metrics page. It reads every tenant's counts, never a
	// job (ADR-1522 scopes job reads, not aggregate counts).
	Stats(ctx context.Context) (Stats, error)
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

	// requeued counts the RUNNING jobs returned to PENDING since the queue
	// opened, by reason (RequeueNodeLost, RequeueRestart, RequeueRollback).
	requeued map[string]uint64

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
		requeued:   make(map[string]uint64),
	}

	// Reload in-flight state from the previous run.
	if err = q.reload(); err != nil {
		return nil, closeAndJoin(db, fmt.Errorf("queue: reload state: %w", err))
	}

	log.Info("job queue opened",
		"db", dbPath,
		"pending", len(q.pendingFIFO),
		"running_reset", q.requeued[RequeueRestart],
	)
	return q, nil
}

// reload reconstructs in-memory state from the database after a restart.
// Running jobs are reset to PENDING (the assigned node is gone).
func (q *SQLiteQueue) reload() error {
	// Reset any RUNNING jobs to PENDING — the nodes that were executing them
	// are no longer connected after a controller restart.
	res, err := q.db.Exec(
		"UPDATE jobs SET status=?, assigned_node=NULL, updated_at=? WHERE status=?",
		StatusPending, time.Now().Unix(), StatusRunning,
	)
	if err != nil {
		return fmt.Errorf("reset running jobs: %w", err)
	}
	reset, err := res.RowsAffected()
	if err != nil {
		return fmt.Errorf("count reset running jobs: %w", err)
	}
	if reset > 0 {
		q.requeued[RequeueRestart] += uint64(reset)
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

// PullWork atomically dequeues the oldest PENDING job of tenantID whose backend
// requirement (if any) is satisfied by the requesting node's capabilities.
// Returns (nil, nil) when no matching job is available.
func (q *SQLiteQueue) PullWork(ctx context.Context, nodeID, tenantID string, capacity NodeCapacity) (*Job, error) {
	q.mu.Lock()
	defer q.mu.Unlock()

	matchIdx, matchID := q.findPendingMatch(tenantID, capacity)
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

	job, err := q.fetchAssignedJob(matchID)
	if err != nil {
		return nil, err
	}

	q.log.Info("job assigned", "job_id", matchID, "node_id", nodeID)
	return job, nil
}

// findPendingMatch returns the FIFO index and id of the oldest PENDING job of tenantID the
// node can run, or (-1, "") when nothing matches. Must be called with q.mu held.
//
// A node that advertises no backends at all is treated as able to run anything, which is
// what keeps a pre-capability node from starving. It is never given another tenant's job
// (ADR-1522).
func (q *SQLiteQueue) findPendingMatch(tenantID string, capacity NodeCapacity) (int, string) {
	backendSet := make(map[string]struct{}, len(capacity.Backends))
	for _, b := range capacity.Backends {
		backendSet[b] = struct{}{}
	}
	for i, id := range q.pendingFIFO {
		job, err := q.getUnlocked(id)
		if err != nil {
			q.log.Warn("queue: failed to fetch pending job", "id", id, "error", err)
			continue
		}
		if job.Status != StatusPending || job.TenantID != tenantID {
			// Stale FIFO entry (job was cancelled externally), or another
			// tenant's job — skip it.
			continue
		}
		// Backend match: if the job specifies a backend, the node must support it.
		if job.Scoring.Backend == "" || len(backendSet) == 0 {
			return i, id
		}
		if _, ok := backendSet[job.Scoring.Backend]; ok {
			return i, id
		}
	}
	return -1, ""
}

// fetchAssignedJob reads back the job PullWork has just moved to RUNNING, undoing the
// assignment when that read fails. Must be called with q.mu held.
//
// By this point the SQL UPDATE has committed (status=running, assigned_node set) and the
// FIFO entry is gone, so all three changes have to be reversed or the job is stranded in
// RUNNING for good. ADR-0961: PullWork rollback on post-update Get failure.
func (q *SQLiteQueue) fetchAssignedJob(matchID string) (*Job, error) {
	job, err := q.getUnlocked(matchID)
	if err == nil {
		return job, nil
	}
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
	q.requeued[RequeueRollback]++
	return nil
}

// Report is one result report of a node: who reports (node and tenant), for
// which job, and with what outcome. Orphaned reports whether a node ID has no
// live session any more; a job assigned to such a node may be reported by
// another session of the same tenant, which is how a node that registered
// again (after a controller restart or an eviction) reports a job it finished
// under its old session (ADR-1524, ADR-1522). Nil means no node is orphaned.
type Report struct {
	NodeID   string
	TenantID string
	JobID    string
	Result   *JobResult
	Orphaned func(nodeID string) bool
}

// ReportResult records the terminal outcome of a job.  If result.Err is
// non-empty the job is marked FAILED; otherwise COMPLETED.
//
// Only these reports are written (ADR-1522):
//   - a job assigned to the reporting node; the UPDATE itself carries
//     assigned_node = node, so nothing else can match it;
//   - a RUNNING job of the reporting tenant whose node is orphaned; the
//     UPDATE compares the status and the tenant, refuses the reporter's own
//     node, and moves the assignment to the reporter in the same statement.
//
// A repeated report of a job that is already terminal and was the reporter's
// (or its orphaned predecessor's) is an idempotent success that reports false.
// Every other report — another tenant's job, a pending job, a job of a live
// node, an unknown job — changes nothing and returns an error wrapping
// ErrNotAssigned.
func (q *SQLiteQueue) ReportResult(ctx context.Context, r Report) (bool, error) {
	status := StatusCompleted
	if r.Result.Err != "" {
		status = StatusFailed
	}
	featuresJSON, err := json.Marshal(r.Result.Features)
	if err != nil {
		// map[string]float64 marshal can only fail on non-finite floats (NaN/Inf);
		// surface the error rather than silently discarding per-feature scores.
		return false, fmt.Errorf("queue: marshal features for job %s: %w", r.JobID, err)
	}
	// ExecContext propagates the caller's ctx so the node's ReportResult RPC
	// deadline / cancellation aborts the UPDATE cleanly.
	// The AND status NOT IN guard makes ReportResult idempotent: a node that
	// retries after a transient gRPC error will not overwrite an already-terminal
	// row, and a Cancel that races with ReportResult cannot be silently undone
	// (r4-retry-idempotency finding).
	res, err := q.db.ExecContext(ctx,
		"UPDATE jobs SET status=?, score=?, features=?, error=?, updated_at=? WHERE id=? AND assigned_node=? AND status NOT IN (?,?,?)",
		status, r.Result.Score, string(featuresJSON), r.Result.Err, time.Now().Unix(), r.JobID, r.NodeID,
		StatusCompleted, StatusFailed, StatusCancelled,
	)
	if err != nil {
		return false, fmt.Errorf("queue: report result for job %s: %w", r.JobID, err)
	}
	if n, _ := res.RowsAffected(); n == 1 {
		q.finishReport(r, status)
		return true, nil
	}
	return q.reportUnassigned(ctx, r, status, string(featuresJSON))
}

// reportUnassigned handles a report the guarded UPDATE did not write: an
// idempotent retry, an orphaned job of the reporter's tenant, or a refusal. It
// reports whether it wrote the result (an adopted orphan).
func (q *SQLiteQueue) reportUnassigned(ctx context.Context, r Report, status, featuresJSON string) (bool, error) {
	switch q.reportDecision(ctx, r) {
	case reportIdempotent:
		q.log.Info("ReportResult: job already in terminal state, ignoring", "job_id", r.JobID)
		return false, nil
	case reportAdopt:
		adopted, err := q.adoptOrphan(ctx, r, status, featuresJSON)
		if err != nil || adopted {
			return adopted, err
		}
	}
	q.log.Warn("ReportResult: refused, job not assigned to node", "job_id", r.JobID, "node_id", r.NodeID)
	return false, fmt.Errorf("queue: job %s: %w", r.JobID, ErrNotAssigned)
}

// adoptOrphan writes the result of an orphaned RUNNING job of the reporter's
// tenant and moves the assignment to the reporter, in one compare-and-set
// UPDATE. It reports whether the row was written.
func (q *SQLiteQueue) adoptOrphan(ctx context.Context, r Report, status, featuresJSON string) (bool, error) {
	res, err := q.db.ExecContext(ctx,
		"UPDATE jobs SET status=?, score=?, features=?, error=?, assigned_node=?, updated_at=? WHERE id=? AND assigned_node<>? AND status=? AND tenant_id=?",
		status, r.Result.Score, featuresJSON, r.Result.Err, r.NodeID, time.Now().Unix(),
		r.JobID, r.NodeID, StatusRunning, r.TenantID,
	)
	if err != nil {
		return false, fmt.Errorf("queue: report orphaned job %s: %w", r.JobID, err)
	}
	if n, _ := res.RowsAffected(); n != 1 {
		return false, nil
	}
	q.log.Info("ReportResult: orphaned job reported by a new session of its tenant",
		"job_id", r.JobID, "node_id", r.NodeID)
	q.finishReport(r, status)
	return true, nil
}

// finishReport drops a reported job from the running set.
func (q *SQLiteQueue) finishReport(r Report, status string) {
	q.mu.Lock()
	delete(q.runningSet, r.JobID)
	q.mu.Unlock()
	q.log.Info("job result recorded", "job_id", r.JobID, "status", status, "score", r.Result.Score)
}

// reportVerdict is what reportDecision decides about a report.
type reportVerdict int

const (
	reportRefuse reportVerdict = iota
	reportIdempotent
	reportAdopt
	reportOwn
)

// reportDecision reads the job and decides a report about it. A read cannot
// widen what the guarded UPDATEs allow: an adoption is written only by its own
// compare-and-set UPDATE, and a node ID that lost its session never gets it
// back (RegisterNode issues a new ID every time).
func (q *SQLiteQueue) reportDecision(ctx context.Context, r Report) reportVerdict {
	var assigned, status, tenant string
	err := q.db.QueryRowContext(ctx,
		"SELECT COALESCE(assigned_node,''), status, COALESCE(tenant_id,'') FROM jobs WHERE id=?", r.JobID,
	).Scan(&assigned, &status, &tenant)
	if err != nil || tenant != r.TenantID || assigned == "" {
		return reportRefuse
	}
	mine := assigned == r.NodeID
	orphaned := !mine && r.Orphaned != nil && r.Orphaned(assigned)
	terminal := status == StatusCompleted || status == StatusFailed || status == StatusCancelled
	switch {
	case terminal && (mine || orphaned):
		return reportIdempotent
	case status == StatusRunning && mine:
		return reportOwn
	case status == StatusRunning && orphaned:
		return reportAdopt
	default:
		return reportRefuse
	}
}

// MayReport reports whether a partial report r would be accepted: the job is
// the reporter's, or a running job of its tenant whose node is orphaned. It
// writes nothing.
func (q *SQLiteQueue) MayReport(ctx context.Context, r Report) bool {
	v := q.reportDecision(ctx, r)
	return v == reportOwn || v == reportAdopt
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

// Cancel marks a PENDING or RUNNING job as CANCELLED and reports true. A job
// already in a terminal state is left alone: Cancel reports false and no error
// (idempotent).
func (q *SQLiteQueue) Cancel(ctx context.Context, jobID string) (bool, error) {
	// ExecContext propagates the caller's ctx so an aborted CancelJob RPC
	// does not leave the UPDATE in flight.
	now := time.Now().Unix()
	res, err := q.db.ExecContext(ctx,
		"UPDATE jobs SET status=?, updated_at=? WHERE id=? AND status IN (?,?)",
		StatusCancelled, now, jobID, StatusPending, StatusRunning,
	)
	if err != nil {
		return false, fmt.Errorf("queue: cancel job %s: %w", jobID, err)
	}

	rows, err := res.RowsAffected()
	if err != nil {
		return false, fmt.Errorf("queue: cancel job %s: count rows: %w", jobID, err)
	}
	if rows == 0 {
		// Job was already terminal — treat as idempotent success.
		q.log.Debug("cancel no-op (already terminal)", "job_id", jobID)
		return false, nil
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
	return true, nil
}

// CancelledAmong returns the entries of ids that name a CANCELLED job of
// tenantID, in the order of ids; it writes nothing. The tenant sits in the SQL
// WHERE clause like ListByTenant's, so a node never learns anything about
// another tenant's job (ADR-1522, ADR-1567).
func (q *SQLiteQueue) CancelledAmong(ctx context.Context, tenantID string, ids []string) ([]string, error) {
	if len(ids) == 0 {
		return nil, nil
	}
	args := make([]any, 0, len(ids)+2)
	args = append(args, tenantID, StatusCancelled)
	for _, id := range ids {
		args = append(args, id)
	}
	// #nosec G202 -- the concatenated fragment is repeatCommaQ output, a pure
	// ",?,?,..." placeholder string; the tenant, the status and every ID bind
	// through `args...`.
	rows, err := q.db.QueryContext(ctx,
		"SELECT id FROM jobs WHERE tenant_id=? AND status=? AND id IN (?"+repeatCommaQ(len(ids)-1)+")", args...)
	if err != nil {
		return nil, fmt.Errorf("queue: look up cancelled jobs: %w", err)
	}
	found, err := q.scanIDs(rows)
	if err != nil {
		return nil, err
	}
	var out []string
	for _, id := range ids {
		if _, ok := found[id]; ok {
			out = append(out, id)
		}
	}
	return out, nil
}

// scanIDs reads a one-column result of job IDs into a set and closes rows.
func (q *SQLiteQueue) scanIDs(rows *sql.Rows) (map[string]struct{}, error) {
	defer func() {
		if closeErr := rows.Close(); closeErr != nil {
			q.log.Warn("queue: close rows", "error", closeErr)
		}
	}()
	found := make(map[string]struct{})
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, fmt.Errorf("queue: scan job id: %w", err)
		}
		found[id] = struct{}{}
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("queue: iterate job ids: %w", err)
	}
	return found, nil
}

// RequeueNode returns the RUNNING jobs assigned to nodeID to PENDING, at the
// front of the FIFO in submission order, and reports how many it moved. A
// node that comes back after its eviction reports such a job under its new
// session; ReportResult's terminal-state guard keeps the first final result.
func (q *SQLiteQueue) RequeueNode(ctx context.Context, nodeID string) (int, error) {
	q.mu.Lock()
	defer q.mu.Unlock()
	ids, err := q.runningJobsOf(ctx, nodeID)
	if err != nil || len(ids) == 0 {
		return 0, err
	}
	_, err = q.db.ExecContext(ctx,
		"UPDATE jobs SET status=?, assigned_node=NULL, updated_at=? WHERE assigned_node=? AND status=?",
		StatusPending, time.Now().Unix(), nodeID, StatusRunning,
	)
	if err != nil {
		return 0, fmt.Errorf("queue: requeue jobs of node %s: %w", nodeID, err)
	}
	for _, id := range ids {
		delete(q.runningSet, id)
	}
	q.pendingFIFO = append(ids, q.pendingFIFO...)
	q.requeued[RequeueNodeLost] += uint64(len(ids))
	q.log.Warn("jobs of evicted node returned to the queue", "node_id", nodeID, "jobs", len(ids))
	return len(ids), nil
}

// runningJobsOf lists the RUNNING jobs assigned to nodeID, oldest first.
// Must be called with q.mu held.
func (q *SQLiteQueue) runningJobsOf(ctx context.Context, nodeID string) ([]string, error) {
	rows, err := q.db.QueryContext(ctx,
		"SELECT id FROM jobs WHERE assigned_node=? AND status=? ORDER BY created_at, rowid",
		nodeID, StatusRunning,
	)
	if err != nil {
		return nil, fmt.Errorf("queue: list jobs of node %s: %w", nodeID, err)
	}
	defer func() {
		if closeErr := rows.Close(); closeErr != nil {
			q.log.Warn("queue: close rows", "error", closeErr)
		}
	}()
	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, fmt.Errorf("queue: scan job of node %s: %w", nodeID, err)
		}
		ids = append(ids, id)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("queue: iterate jobs of node %s: %w", nodeID, err)
	}
	return ids, nil
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

// ListByTenant returns a point-in-time snapshot of the jobs of tenantID,
// optionally filtered to the provided statuses.  An empty statuses slice
// returns every job of the tenant.  The tenant is part of the SQL WHERE
// clause: no other tenant's row is ever read (ADR-1522).
//
// Callers receive copies — mutations to the returned slice do not affect the
// queue.  Used by controllerServer.StreamJobs to send a consistent snapshot
// (ADR-0962).
func (q *SQLiteQueue) ListByTenant(ctx context.Context, tenantID string, statuses []string) ([]*Job, error) {
	rows, err := q.queryJobs(ctx, tenantID, statuses)
	if err != nil {
		return nil, fmt.Errorf("queue: list jobs of tenant %q: %w", tenantID, err)
	}
	defer func() {
		if closeErr := rows.Close(); closeErr != nil {
			q.log.Warn("queue: close ListByTenant rows", "error", closeErr)
		}
	}()

	var out []*Job
	for rows.Next() {
		job, scanErr := scanJobRow(rows)
		if scanErr != nil {
			return nil, scanErr
		}
		out = append(out, job)
	}
	if err = rows.Err(); err != nil {
		return nil, fmt.Errorf("queue: iterate jobs in ListByTenant: %w", err)
	}
	return out, nil
}

// queryJobs runs the ListByTenant SELECT for tenantID, narrowed to statuses when any
// are given.
func (q *SQLiteQueue) queryJobs(ctx context.Context, tenantID string, statuses []string) (*sql.Rows, error) {
	const columns = "SELECT id, status, scoring, COALESCE(assigned_node,''), " +
		"COALESCE(score,0), COALESCE(features,'{}'), COALESCE(error,''), " +
		"COALESCE(tenant_id,''), created_at, updated_at FROM jobs WHERE tenant_id = ?"
	if len(statuses) == 0 {
		return q.db.QueryContext(ctx, columns+" ORDER BY created_at ASC", tenantID)
	}
	// Build a parameterised IN clause.  We limit statuses to the known set
	// (max 5) so the query never becomes unbounded.
	args := make([]any, 0, len(statuses)+1)
	args = append(args, tenantID)
	for _, s := range statuses {
		args = append(args, s)
	}
	// #nosec G202 -- The concatenated fragment is repeatCommaQ output, a
	// pure ",?,?,..." placeholder string of length len(statuses)-1; no
	// user data enters the SQL text. The tenant and the status values bind
	// through `args...` as parameterised arguments.
	query := columns + " AND status IN (?" + repeatCommaQ(len(statuses)-1) + ") ORDER BY created_at ASC"
	return q.db.QueryContext(ctx, query, args...)
}

// scanJobRow decodes one ListByTenant row into a freshly allocated Job, so callers own the
// value rather than aliasing the loop variable.
//
// Unreadable features decode to an empty map rather than failing the whole snapshot: the
// features are reporting detail, while the scoring request is the job's identity and a
// job whose scoring cannot be read is not reportable at all.
func scanJobRow(rows *sql.Rows) (*Job, error) {
	var (
		job          Job
		scoringJSON  string
		featuresJSON string
		createdAt    int64
		updatedAt    int64
	)
	if err := rows.Scan(
		&job.ID, &job.Status, &scoringJSON, &job.AssignedNode,
		&job.Score, &featuresJSON, &job.Error, &job.TenantID,
		&createdAt, &updatedAt,
	); err != nil {
		return nil, fmt.Errorf("queue: scan job row in ListByTenant: %w", err)
	}
	if err := json.Unmarshal([]byte(scoringJSON), &job.Scoring); err != nil {
		return nil, fmt.Errorf("queue: unmarshal scoring in ListByTenant: %w", err)
	}
	if err := json.Unmarshal([]byte(featuresJSON), &job.Features); err != nil {
		job.Features = map[string]float64{}
	}
	job.CreatedAt = time.Unix(createdAt, 0)
	job.UpdatedAt = time.Unix(updatedAt, 0)
	return &job, nil
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
