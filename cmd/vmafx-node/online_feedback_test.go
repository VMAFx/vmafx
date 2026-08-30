// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/online_feedback_test.go — regression tests covering the
// drainer-lifetime contract for FeedbackClient.
//
// Background: the drainer is launched by Start() (wired to an fx OnStart hook)
// and stopped + awaited by Close() (wired to an fx OnStop hook) — ADR-1119. The
// constructor no longer spawns a goroutine and no longer takes a context; the
// drainer is bound to an internal, Close-owned context so no goroutine leaks
// past Close(). This suite locks in Start/Close idempotency, the
// Close-without-Start and Start-after-Close edge cases, and the nil-logger
// constructor guard.
package main

import (
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
// Close must still return promptly.
func TestFeedbackClient_CloseStopsDrainer(t *testing.T) {
	t.Parallel()

	fc := NewFeedbackClient(nil)
	fc.Start()

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

// TestFeedbackClient_CloseWithoutStart verifies that Close() on a client whose
// drainer was never started returns immediately without hanging on the (never
// closed) done channel.
func TestFeedbackClient_CloseWithoutStart(t *testing.T) {
	t.Parallel()

	fc := NewFeedbackClient(nil)
	done := make(chan struct{})
	go func() {
		fc.Close()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("Close() without Start blocked — must not wait on an unstarted drainer")
	}
}

// TestFeedbackClient_StartAfterClose verifies that calling Start() after Close()
// does not launch a goroutine and does not panic (no double-close of done).
func TestFeedbackClient_StartAfterClose(t *testing.T) {
	t.Parallel()

	fc := NewFeedbackClient(nil)
	fc.Close()
	// Start after Close must be a safe no-op.
	fc.Start()
	// A trailing Close must still return immediately.
	done := make(chan struct{})
	go func() {
		fc.Close()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("Close() after Start-after-Close blocked")
	}
}

// TestFeedbackClient_CloseIdempotent verifies that calling Close() multiple
// times — including concurrently — is safe.
func TestFeedbackClient_CloseIdempotent(t *testing.T) {
	t.Parallel()

	fc := NewFeedbackClient(nil)
	fc.Start()

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

// TestFeedbackClient_StartIdempotent verifies that calling Start() multiple
// times only launches the drainer once (no double-close of done on shutdown).
func TestFeedbackClient_StartIdempotent(t *testing.T) {
	t.Parallel()

	fc := NewFeedbackClient(nil)
	fc.Start()
	fc.Start()
	fc.Start()
	defer fc.Close()
}

// TestFeedbackClient_NilLoggerDoesNotPanic verifies that the constructor
// accepts a nil logger and the drainer's first log call does not panic
// (it previously dereferenced a nil *slog.Logger inside drainLoop).
func TestFeedbackClient_NilLoggerDoesNotPanic(t *testing.T) {
	t.Parallel()

	fc := NewFeedbackClient(nil)
	fc.Start()
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
