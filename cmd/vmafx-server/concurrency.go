// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/concurrency.go — per-server concurrency cap for Score RPCs.
//
// Background: Scorer.Score forks a full vmaf subprocess per call. Without a
// cap, N concurrent unauthenticated POSTs/RPCs → N vmaf processes, which
// exhausts CPU/RAM/PIDs (unauthenticated DoS). This file provides the shared
// ScoreLimiter that both the HTTP and gRPC handlers use to bound in-flight
// scoring operations.
//
// The limiter is backed by golang.org/x/sync/semaphore.Weighted, which is
// already a declared dependency. Acquire(ctx) propagates context cancellation
// so a timed-out or disconnected client is drained immediately rather than
// sleeping in the queue.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.

//go:build cgo

package main

import (
	"context"
	"fmt"

	"golang.org/x/sync/semaphore"
)

// ScoreLimiter is a counting semaphore that caps simultaneous in-flight calls
// to Scorer.Score. It is shared between the HTTP and gRPC handler paths so
// the limit is enforced system-wide (not per-protocol).
type ScoreLimiter struct {
	sem *semaphore.Weighted
	max int64
}

// NewScoreLimiter creates a ScoreLimiter with the given maximum concurrency.
// max must be >= 1.
func NewScoreLimiter(max int) (*ScoreLimiter, error) {
	if max < 1 {
		return nil, fmt.Errorf("concurrency: max must be >= 1, got %d", max)
	}
	return &ScoreLimiter{
		sem: semaphore.NewWeighted(int64(max)),
		max: int64(max),
	}, nil
}

// Acquire acquires one slot from the semaphore. It blocks until a slot is
// available or ctx is cancelled. Returns an error if ctx is cancelled before
// a slot is acquired (i.e. capacity is full and the request should be rejected).
func (l *ScoreLimiter) Acquire(ctx context.Context) error {
	return l.sem.Acquire(ctx, 1)
}

// Release returns one slot to the semaphore. Must be called after every
// successful Acquire, typically via defer.
func (l *ScoreLimiter) Release() {
	l.sem.Release(1)
}

// Max returns the configured maximum concurrency.
func (l *ScoreLimiter) Max() int64 {
	return l.max
}
