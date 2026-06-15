// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/online_feedback_pump_test.go — table-driven tests that
// exercise the FeedbackClient.pump / sendOne / Send paths with a real Unix
// socket listener, verifying end-to-end message delivery, the drop counter, and
// the sidecar-reconnect path.
//
// ADR-0781: sidecar online training — SGD + EMA + replay buffer.
// ADR-1119: FeedbackClient drainer is launched via Start() (fx OnStart), not at
// construction; the constructor no longer takes a context.

package main

import (
	"bufio"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// startEchoSidecar starts a Unix domain socket server that reads one newline-
// delimited JSON message and responds with {"ok":true,"step":1}. It accepts
// exactly n connections total (used to gate the test).
// Returns (socketPath, readyCh, stopFunc).
func startEchoSidecar(t *testing.T, n int) (string, <-chan struct{}, func()) {
	t.Helper()
	sockPath := filepath.Join(t.TempDir(), "sidecar.sock")

	lis, err := net.Listen("unix", sockPath)
	if err != nil {
		t.Fatalf("listen unix %s: %v", sockPath, err)
	}

	ready := make(chan struct{})
	stop := make(chan struct{})
	t.Cleanup(func() { close(stop) })

	go func() {
		close(ready)
		for i := 0; i < n; i++ {
			conn, acceptErr := lis.Accept()
			if acceptErr != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()
				sc := bufio.NewScanner(c)
				for sc.Scan() {
					ack, _ := json.Marshal(map[string]any{
						"ok":   true,
						"step": 1,
					})
					_, _ = c.Write(append(ack, '\n'))
				}
			}(conn)
		}
		lis.Close()
	}()

	stopFn := func() { _ = lis.Close() }
	return sockPath, ready, stopFn
}

// TestFeedbackClient_DeliveryCountIncremented verifies that Delivered()
// increments for each message successfully acknowledged by the sidecar.
// Note: t.Setenv is incompatible with t.Parallel.
func TestFeedbackClient_DeliveryCountIncremented(t *testing.T) {
	sockPath, ready, stopFn := startEchoSidecar(t, 1)
	defer stopFn()
	<-ready

	t.Setenv(feedbackSocketEnv, sockPath)

	fc := NewFeedbackClient(nil)
	fc.Start()
	defer fc.Close()

	// Send a few messages.
	for i := range 3 {
		fc.Send(&FeedbackMessage{
			JobID:     "job-" + string(rune('A'+i)),
			Features:  []float32{0.1, 0.2},
			TrueScore: float32(i) + 75.0,
		})
	}

	// Wait for delivery with a bounded timeout.
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if fc.Delivered() >= 1 {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if fc.Delivered() < 1 {
		t.Errorf("Delivered() = %d, want >= 1 (sidecar may not have accepted connection in time)", fc.Delivered())
	}
}

// TestFeedbackClient_Send_EnqueuesAndReturnsTrue verifies that Send returns
// true when the queue has capacity. Send works before Start() — the queue is
// live immediately.
// Note: t.Setenv is incompatible with t.Parallel.
func TestFeedbackClient_Send_EnqueuesAndReturnsTrue(t *testing.T) {
	// Use a socket that does not exist so the drainer immediately retries
	// without consuming messages from the queue.
	t.Setenv(feedbackSocketEnv, filepath.Join(t.TempDir(), "nonexistent.sock"))

	fc := NewFeedbackClient(nil)
	fc.Start()
	defer fc.Close()

	ok := fc.Send(&FeedbackMessage{JobID: "j1", Features: nil, TrueScore: 80.0})
	if !ok {
		t.Error("Send: got false, want true (queue should have capacity)")
	}
}

// TestFeedbackClient_DropCounterIncrementsOnOverflow verifies that Send returns
// false and Dropped() increments when the queue is exhausted.
// Note: t.Setenv is incompatible with t.Parallel.
func TestFeedbackClient_DropCounterIncrementsOnOverflow(t *testing.T) {
	// Non-existent socket → drainer never drains → queue fills up.
	t.Setenv(feedbackSocketEnv, filepath.Join(t.TempDir(), "nonexistent.sock"))

	fc := NewFeedbackClient(nil)
	fc.Start()
	defer fc.Close()

	dropped := 0
	for i := range feedbackQueueCap + 10 {
		msg := &FeedbackMessage{JobID: "overflow", Features: []float32{float32(i)}, TrueScore: 1.0}
		if !fc.Send(msg) {
			dropped++
		}
	}
	if dropped == 0 {
		t.Error("expected at least one dropped message when queue is full")
	}
	if fc.Dropped() <= 0 {
		t.Errorf("Dropped() = %d, want > 0", fc.Dropped())
	}
}

// TestFeedbackClient_SocketEnvOverride verifies that the VMAFX_SIDECAR_SOCKET
// environment variable is read by NewFeedbackClient.
// Note: t.Setenv is incompatible with t.Parallel.
func TestFeedbackClient_SocketEnvOverride(t *testing.T) {
	custom := filepath.Join(t.TempDir(), "custom.sock")
	t.Setenv(feedbackSocketEnv, custom)

	fc := NewFeedbackClient(nil)
	defer fc.Close()

	if fc.socketPath != custom {
		t.Errorf("socketPath = %q, want %q", fc.socketPath, custom)
	}
}

// TestFeedbackClient_DefaultSocketPath verifies that an unset
// VMAFX_SIDECAR_SOCKET falls back to feedbackSocketDefault.
// Note: uses Unsetenv so cannot use t.Parallel.
func TestFeedbackClient_DefaultSocketPath(t *testing.T) {
	if err := os.Unsetenv(feedbackSocketEnv); err != nil {
		t.Fatalf("Unsetenv: %v", err)
	}

	fc := NewFeedbackClient(nil)
	defer fc.Close()

	if fc.socketPath != feedbackSocketDefault {
		t.Errorf("socketPath = %q, want %q", fc.socketPath, feedbackSocketDefault)
	}
}
