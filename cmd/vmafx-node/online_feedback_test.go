// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/online_feedback_test.go — regression tests covering the
// drainer-lifetime contract for FeedbackClient.
//
// Background: prior to this audit the FeedbackClient documentation promised
// a Close() method that stopped the background drainer, but the method was
// never implemented — only ctx cancellation could reap the goroutine. This
// suite locks in the documented Close() behaviour and the nil-logger
// constructor guard.
package main

import (
	"context"
	"sync"
	"testing"
	"time"
)

// TestFeedbackClient_CloseStopsDrainer verifies that Close() returns only
// after the background drainer goroutine has exited.
//
// We don't compare goroutine counts directly — runtime.NumGoroutine is
// influenced by parallel tests sharing the process and produces noisy
// baselines under t.Parallel.  Instead we verify the documented contract:
// Close blocks until the drainer's done channel closes, and a second
// Close (or a Close after ctx-cancel) must still return promptly.
func TestFeedbackClient_CloseStopsDrainer(t *testing.T) {
	t.Parallel()

	fc := NewFeedbackClient(context.Background(), nil)

	done := make(chan struct{})
	go func() {
		fc.Close()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("Close() did not return within 2s — drainer goroutine is stuck")
	}

	// A follow-up Close after the drainer has exited must also be
	// instantaneous (idempotent).
	postClose := time.Now()
	fc.Close()
	if d := time.Since(postClose); d > 50*time.Millisecond {
		t.Errorf("second Close() took %v — expected near-instant return on a closed drainer", d)
	}
}

// TestFeedbackClient_CloseIdempotent verifies that calling Close() multiple
// times — including concurrently — is safe.
func TestFeedbackClient_CloseIdempotent(t *testing.T) {
	t.Parallel()

	fc := NewFeedbackClient(context.Background(), nil)

	var wg sync.WaitGroup
	for range 8 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			fc.Close()
		}()
	}
	wg.Wait()
	// A trailing serial Close must also return without panic or hang.
	fc.Close()
}

// TestFeedbackClient_NilLoggerDoesNotPanic verifies that the constructor
// accepts a nil logger and the drainer's first log call does not panic
// (it previously dereferenced a nil *slog.Logger inside drainLoop).
func TestFeedbackClient_NilLoggerDoesNotPanic(t *testing.T) {
	t.Parallel()

	fc := NewFeedbackClient(context.Background(), nil)
	defer fc.Close()

	// Trigger the drop path so log.Debug is exercised on a nil-input
	// constructor with a substituted slog.Default(). If the substitution
	// is missing this either panics or deadlocks.
	for range feedbackQueueCap + 4 {
		fc.Send(&FeedbackMessage{JobID: "test"})
	}
	if got := fc.Dropped(); got == 0 {
		t.Errorf("expected at least one drop after overfilling the queue, got %d", got)
	}
}

// TestFeedbackClient_CtxCancelStopsDrainer verifies that cancelling the
// constructor context also stops the drainer, independent of Close().
func TestFeedbackClient_CtxCancelStopsDrainer(t *testing.T) {
	t.Parallel()

	ctx, cancel := context.WithCancel(context.Background())
	fc := NewFeedbackClient(ctx, nil)

	cancel()
	// Close still drains the done channel — must not block forever even
	// though ctx, not Close, stopped the goroutine.
	doneCh := make(chan struct{})
	go func() {
		fc.Close()
		close(doneCh)
	}()
	select {
	case <-doneCh:
	case <-time.After(2 * time.Second):
		t.Fatal("Close() blocked after ctx cancel — drainer never exited")
	}
}
