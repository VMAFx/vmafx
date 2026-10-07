// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/backend/backend.go — what the controller's gRPC and
// HTTP handlers need from the place jobs and node sessions live.

// Package backend is the seam between the controller's handlers and where its
// state lives (ADR-2350). Postgres keeps jobs and node sessions in
// PostgreSQL through the store package, so several controller replicas serve
// any node; the embedded SQLite queue of ADR-1119 stays behind the same
// interface until the SQLite store of the standalone profile replaces it.
//
// The node protocol (controller.proto) is unchanged: the node ID a node
// receives at registration names its session, and the attempt a write
// belongs to is the one the session holds (ADR-2350 D2).
package backend

import (
	"context"
	"errors"
	"time"
)

// The job states, as the controller reports them.
const (
	StatusPending   = "pending"
	StatusRunning   = "running"
	StatusCompleted = "completed"
	StatusFailed    = "failed"
	StatusCancelled = "cancelled"
)

// Errors every backend returns (wrapped).
var (
	// ErrNotFound is a job that does not exist or belongs to another tenant.
	ErrNotFound = errors.New("backend: job not found")
	// ErrInvalidSession is a node session that is unknown, expired, of
	// another tenant, or presented with the wrong token.
	ErrInvalidSession = errors.New("backend: node session unknown, expired or not the caller's")
	// ErrNotAssigned is a report about a job the session does not run.
	ErrNotAssigned = errors.New("backend: job is not assigned to this node")
)

// Scoring is what a job scores (controller.proto ScoringParams).
type Scoring struct {
	Reference string `json:"reference"`
	Distorted string `json:"distorted"`
	Model     string `json:"model"`
	Backend   string `json:"backend"`
}

// Job is a snapshot of one job.
type Job struct {
	ID           string
	TenantID     string
	Status       string
	Scoring      Scoring
	AssignedNode string
	Score        float64
	Features     map[string]float64
	Error        string
	CreatedAt    time.Time
	UpdatedAt    time.Time
}

// Capability is what a node can run (controller.proto NodeCapability).
type Capability struct {
	GPUVendor   string   `json:"gpu_vendor,omitempty"`
	Backends    []string `json:"backends,omitempty"`
	Concurrency int      `json:"concurrency,omitempty"`
}

// Session identifies a node session in its calls.
type Session struct {
	NodeID string
	Token  string
}

// Result is a node's report about a job; a non-empty Err fails it.
type Result struct {
	Score    float64
	Features map[string]float64
	Err      string
}

// TenantCount is one tenant's live job counts.
type TenantCount struct {
	TenantID      string
	Pending       int
	Running       int
	OldestPending time.Time
}

// Stats are a backend's live counts for the /metrics page.
type Stats struct {
	Tenants   []TenantCount
	LiveNodes int
}

// Backend is where the controller keeps jobs and node sessions. Every method
// takes the tenant of the authenticated caller and never reads or writes
// another tenant's data (ADR-1522).
type Backend interface {
	// Submit enqueues a job and returns its ID.
	Submit(ctx context.Context, tenantID string, s Scoring) (string, error)
	// Get returns a job, or ErrNotFound.
	Get(ctx context.Context, tenantID, jobID string) (*Job, error)
	// Cancel cancels a pending or running job and reports whether it did;
	// a terminal job is left alone (false, nil); ErrNotFound otherwise.
	Cancel(ctx context.Context, tenantID, jobID string) (bool, error)
	// List returns the tenant's jobs, optionally filtered by status.
	List(ctx context.Context, tenantID string, statuses []string) ([]*Job, error)
	// Register opens a node session.
	Register(ctx context.Context, tenantID, name string, c Capability) (Session, error)
	// Heartbeat renews a session and the leases of the jobs it runs, and
	// returns those of running that were cancelled; ErrInvalidSession when
	// the session is not the caller's.
	Heartbeat(ctx context.Context, tenantID string, s Session, running []string) ([]string, error)
	// Pull hands the session the next job it can run, or nil.
	Pull(ctx context.Context, tenantID string, s Session, c Capability) (*Job, error)
	// Report records a node's final result and reports whether this call
	// recorded it (a retry of an accepted report is (false, nil));
	// ErrNotAssigned when the session does not run the job.
	Report(ctx context.Context, tenantID string, s Session, jobID string, r Result) (bool, error)
	// MayReport reports whether the session runs the job now; it writes
	// nothing. Partial reports are accepted only then.
	MayReport(ctx context.Context, tenantID string, s Session, jobID string) (bool, error)
	// Stats returns the live counts for the /metrics page.
	Stats(ctx context.Context) (Stats, error)
	// Ready returns nil when the backend can serve requests.
	Ready(ctx context.Context) error
}
