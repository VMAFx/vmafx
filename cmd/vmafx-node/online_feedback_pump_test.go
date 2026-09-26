// SPDX-License-Identifier: EUPL-1.2
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
	"context"
	"encoding/json"
	"math"
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
		for range n {
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

// TestFeedbackAckRetryQueuedFields pins the Python sidecar's admission-aware
// failure ACK so the Go client cannot silently drift from the wire contract.
func TestFeedbackAckRetryQueuedFields(t *testing.T) {
	var ack feedbackAck
	err := json.Unmarshal(
		[]byte(`{"ok":true,"trained":false,"retry_queued":true,"training_error":"oom"}`),
		&ack,
	)
	if err != nil {
		t.Fatalf("unmarshal retry-queued ACK: %v", err)
	}
	if !ack.OK || ack.Trained || !ack.RetryQueued || ack.TrainingError != "oom" {
		t.Fatalf("retry-queued ACK decoded incorrectly: %+v", ack)
	}
}

// TestFeedbackClient_RetryQueuedAckIsAccepted verifies that an admitted sample
// remains accepted even when its gradient step is deferred for retry.
func TestFeedbackClient_RetryQueuedAckIsAccepted(t *testing.T) {
	client, server := net.Pipe()
	t.Cleanup(func() {
		_ = client.Close()
		_ = server.Close()
	})

	fc := NewFeedbackClient(nil)
	t.Cleanup(fc.Close)
	msg := &FeedbackMessage{JobID: "accepted", Features: []float32{0.1}, TrueScore: 75.0}

	go func() {
		scanner := bufio.NewScanner(server)
		if !scanner.Scan() {
			return
		}
		_, _ = server.Write([]byte(
			`{"ok":true,"trained":false,"retry_queued":true,"training_error":"oom"}` + "\n",
		))
	}()

	disposition, err := fc.sendOne(client, bufio.NewReader(client), msg)
	if err != nil {
		t.Fatalf("sendOne returned error for admitted retry-queued sample: %v", err)
	}
	if disposition != feedbackSendAccepted {
		t.Fatalf("sendOne disposition = %d, want accepted", disposition)
	}
}

// TestFeedbackClient_RetryableRejectionRetainsWithoutDelivery verifies that
// sidecar backpressure leaves the unadmitted sample pending for reconnect and
// does not inflate the delivered counter.
func TestFeedbackClient_RetryableRejectionRetainsWithoutDelivery(t *testing.T) {
	client, server := net.Pipe()
	t.Cleanup(func() {
		_ = client.Close()
		_ = server.Close()
	})

	fc := NewFeedbackClient(nil)
	t.Cleanup(fc.Close)
	msg := &FeedbackMessage{JobID: "retry-me", Features: []float32{0.1}, TrueScore: 75.0}
	fc.queue <- msg

	go func() {
		scanner := bufio.NewScanner(server)
		if !scanner.Scan() {
			return
		}
		_, _ = server.Write([]byte(
			`{"ok":false,"retryable":true,"error":"pending training queue is full"}` + "\n",
		))
	}()

	ctx, cancel := context.WithTimeout(t.Context(), 250*time.Millisecond)
	defer cancel()
	pending, err := fc.pump(ctx, client, nil)
	if err == nil {
		t.Fatal("pump returned nil for retryable sidecar rejection")
	}
	if got := fc.Delivered(); got != 0 {
		t.Fatalf("Delivered() = %d after retryable rejection, want 0", got)
	}
	if pending != msg {
		t.Fatalf("pending retry = %p, want original %p", pending, msg)
	}
	if got := len(fc.queue); got != 0 {
		t.Fatalf("queue length = %d after retaining retry, want 0", got)
	}
}

// TestFeedbackClient_RetryableRejectionSurvivesFullQueueAcrossReconnect locks
// in the race where another producer refills the bounded queue while a sample
// is waiting for its sidecar ACK. The rejected sample remains the reconnect
// priority even though there is no queue slot available for it.
func TestFeedbackClient_RetryableRejectionSurvivesFullQueueAcrossReconnect(t *testing.T) {
	sockPath := filepath.Join(t.TempDir(), "sidecar.sock")
	lis, err := net.Listen("unix", sockPath)
	if err != nil {
		t.Fatalf("listen unix %s: %v", sockPath, err)
	}
	t.Cleanup(func() { _ = lis.Close() })
	t.Setenv(feedbackSocketEnv, sockPath)

	fc := NewFeedbackClient(nil)
	t.Cleanup(func() {
		_ = lis.Close()
		fc.Close()
	})

	retry := &FeedbackMessage{
		JobID:     "retry-me",
		Features:  []float32{0.1},
		TrueScore: 75.0,
	}
	if !fc.Send(retry) {
		t.Fatal("initial retry candidate was not enqueued")
	}
	fc.Start()

	accept := func() net.Conn {
		t.Helper()
		unixLis, ok := lis.(*net.UnixListener)
		if !ok {
			t.Fatalf("listener type = %T, want *net.UnixListener", lis)
		}
		if deadlineErr := unixLis.SetDeadline(time.Now().Add(2 * time.Second)); deadlineErr != nil {
			t.Fatalf("set accept deadline: %v", deadlineErr)
		}
		conn, acceptErr := unixLis.Accept()
		if acceptErr != nil {
			t.Fatalf("accept sidecar connection: %v", acceptErr)
		}
		return conn
	}
	readMessage := func(conn net.Conn) FeedbackMessage {
		t.Helper()
		if deadlineErr := conn.SetReadDeadline(time.Now().Add(2 * time.Second)); deadlineErr != nil {
			t.Fatalf("set message read deadline: %v", deadlineErr)
		}
		line, readErr := bufio.NewReader(conn).ReadBytes('\n')
		if readErr != nil {
			t.Fatalf("read feedback message: %v", readErr)
		}
		var msg FeedbackMessage
		if jsonErr := json.Unmarshal(line, &msg); jsonErr != nil {
			t.Fatalf("decode feedback message: %v", jsonErr)
		}
		return msg
	}

	firstConn := accept()
	first := readMessage(firstConn)
	if first.JobID != retry.JobID {
		t.Fatalf("first connection received job_id %q, want %q", first.JobID, retry.JobID)
	}

	// The retry candidate is now in flight and the queue slot it occupied is
	// available. Refill every slot before the sidecar rejects the sample.
	for i := range feedbackQueueCap {
		if !fc.Send(&FeedbackMessage{JobID: "filler", TrueScore: float32(i)}) {
			t.Fatalf("filler %d was dropped before the queue reached capacity", i)
		}
	}
	if got := len(fc.queue); got != feedbackQueueCap {
		t.Fatalf("queue length = %d, want full capacity %d", got, feedbackQueueCap)
	}

	if _, writeErr := firstConn.Write([]byte(
		`{"ok":false,"retryable":true,"error":"pending training queue is full"}` + "\n",
	)); writeErr != nil {
		t.Fatalf("write retryable ACK: %v", writeErr)
	}
	_ = firstConn.Close()

	secondConn := accept()
	defer secondConn.Close()
	second := readMessage(secondConn)
	if second.JobID != retry.JobID {
		t.Fatalf("first message after reconnect = %q, want retained %q", second.JobID, retry.JobID)
	}
	if _, writeErr := secondConn.Write([]byte(`{"ok":true,"step":1}` + "\n")); writeErr != nil {
		t.Fatalf("write successful ACK: %v", writeErr)
	}

	deadline := time.Now().Add(2 * time.Second)
	for fc.Delivered() != 1 && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	if got := fc.Delivered(); got != 1 {
		t.Fatalf("Delivered() = %d, want 1 after retained retry succeeds", got)
	}
	if got := fc.Dropped(); got != 0 {
		t.Fatalf("Dropped() = %d, want 0; an in-flight retry must not compete for a queue slot", got)
	}
}

// TestFeedbackClient_PermanentEncodeFailureDoesNotStarveQueue verifies that a
// locally unencodable message is terminal: it is counted as dropped, does not
// force a reconnect, and cannot prevent the next valid message from draining.
func TestFeedbackClient_PermanentEncodeFailureDoesNotStarveQueue(t *testing.T) {
	tests := []struct {
		name  string
		value float32
	}{
		{name: "nan", value: float32(math.NaN())},
		{name: "positive-infinity", value: float32(math.Inf(1))},
		{name: "negative-infinity", value: float32(math.Inf(-1))},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			sockPath, ready, stopFn := startEchoSidecar(t, 1)
			defer stopFn()
			<-ready
			t.Setenv(feedbackSocketEnv, sockPath)

			fc := NewFeedbackClient(nil)
			defer fc.Close()
			if !fc.Send(&FeedbackMessage{JobID: "poison", TrueScore: tt.value}) {
				t.Fatal("poison message was not enqueued")
			}
			if !fc.Send(&FeedbackMessage{JobID: "valid", TrueScore: 75.0}) {
				t.Fatal("valid message was not enqueued")
			}
			fc.Start()

			deadline := time.Now().Add(time.Second)
			for fc.Delivered() != 1 && time.Now().Before(deadline) {
				time.Sleep(time.Millisecond)
			}
			if got := fc.Delivered(); got != 1 {
				t.Fatalf("Delivered() = %d, want 1; poison message starved valid feedback", got)
			}
			if got := fc.Dropped(); got != 1 {
				t.Fatalf("Dropped() = %d, want 1 terminal local encode failure", got)
			}
		})
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
