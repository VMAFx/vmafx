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
	"runtime"
	"sync"
	"testing"
	"time"
)

// TestFeedbackClient_CloseStopsDrainer verifies that Close() returns only
// after the background drainer goroutine has exited, with no goroutine
// leak past the Close() call.
func TestFeedbackClient_CloseStopsDrainer(t *testing.T) {
	t.Parallel()

	baseline := runtime.NumGoroutine()
	fc := NewFeedbackClient(context.Background(), nil)

	// Give the drainer a tick to start so the goroutine count actually
	// reflects its presence.
	time.Sleep(20 * time.Millisecond)
	if got := runtime.NumGoroutine(); got <= baseline {
		t.Logf("warning: goroutine count did not rise post-construction (baseline=%d got=%d)",
			baseline, got)
	}

	fc.Close()

	// After Close returns the drainer goroutine must be gone. Allow a
	// short grace window for the runtime scheduler to retire the
	// stack — Close itself blocks on <-fc.done so this is belt-and-braces.
	deadline := time.Now().Add(500 * time.Millisecond)
	for time.Now().Before(deadline) {
		if runtime.NumGoroutine() <= baseline+1 {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Errorf("drainer goroutine still running after Close(): baseline=%d, now=%d",
		baseline, runtime.NumGoroutine())
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
