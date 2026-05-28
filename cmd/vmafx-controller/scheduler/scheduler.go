// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/scheduler/scheduler.go — work scheduler.
//
// The Scheduler mediates between the job queue and the node registry.
// Its primary entry point is Assign(nodeID, token, capability), called
// by the gRPC PullWork handler.
//
// Scheduling policy (Phase 4b.1 — simple FIFO + capability match):
//   - The oldest PENDING job whose backend requirement is satisfied by the
//     requesting node's capabilities is selected (implemented in queue.PullWork).
//   - Round-robin fairness among nodes with equal capabilities is a later
//     concern; for now FIFO order among jobs is sufficient.
//
// The Scheduler holds no state of its own; it is a thin coordination layer
// over queue.Queue and nodes.Registry.
//
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.

package scheduler

import (
	"context"
	"fmt"
	"log/slog"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
)

// Scheduler coordinates job dispatch to vmafx-node workers.
type Scheduler struct {
	queue    queue.Queue
	registry *nodes.Registry
	log      *slog.Logger
}

// New creates a Scheduler backed by the given queue and registry.
func New(q queue.Queue, r *nodes.Registry, log *slog.Logger) *Scheduler {
	return &Scheduler{queue: q, registry: r, log: log}
}

// Assign pulls the next matching PENDING job for the given node and transitions
// it to RUNNING.  Returns (nil, nil) when no matching job is available.
//
// Returns an error if the node session is invalid.
func (s *Scheduler) Assign(ctx context.Context, nodeID, sessionToken string, cap nodes.Capability) (*queue.Job, error) {
	if !s.registry.ValidateSession(nodeID, sessionToken) {
		return nil, fmt.Errorf("scheduler: unknown or expired session for node %q", nodeID)
	}

	qCap := queue.NodeCapacity{
		Backends: cap.Backends,
		Slots:    cap.Concurrency,
	}

	job, err := s.queue.PullWork(ctx, nodeID, qCap)
	if err != nil {
		return nil, fmt.Errorf("scheduler: pull work: %w", err)
	}
	if job != nil {
		s.log.Info("scheduler assigned job",
			"job_id", job.ID,
			"node_id", nodeID,
			"backend", job.Scoring.Backend,
		)
	}
	return job, nil
}
