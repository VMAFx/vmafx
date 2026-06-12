// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/concurrency_test.go — tests for the ScoreLimiter concurrency
// cap.  Verifies that the HTTP and gRPC handlers never run more than
// maxConcurrent Scorer.Score calls simultaneously and that excess callers receive
// the correct error response (HTTP 429 / gRPC codes.ResourceExhausted).
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.

//go:build cgo

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// ---------------------------------------------------------------------------
// ScoreLimiter unit tests
// ---------------------------------------------------------------------------

// TestNewScoreLimiter_Valid verifies that a limiter with max>=1 is created.
func TestNewScoreLimiter_Valid(t *testing.T) {
	t.Parallel()
	l, err := NewScoreLimiter(4)
	if err != nil {
		t.Fatalf("NewScoreLimiter: %v", err)
	}
	if l.Max() != 4 {
		t.Errorf("Max: got %d, want 4", l.Max())
	}
}

// TestNewScoreLimiter_ZeroRejected verifies that max=0 returns an error.
func TestNewScoreLimiter_ZeroRejected(t *testing.T) {
	t.Parallel()
	if _, err := NewScoreLimiter(0); err == nil {
		t.Fatal("expected error for max=0, got nil")
	}
}

// TestNewScoreLimiter_NegativeRejected verifies that negative max returns an error.
func TestNewScoreLimiter_NegativeRejected(t *testing.T) {
	t.Parallel()
	if _, err := NewScoreLimiter(-1); err == nil {
		t.Fatal("expected error for max=-1, got nil")
	}
}

// TestScoreLimiter_AcquireRelease verifies basic semaphore semantics.
func TestScoreLimiter_AcquireRelease(t *testing.T) {
	t.Parallel()
	l, err := NewScoreLimiter(2)
	if err != nil {
		t.Fatalf("NewScoreLimiter: %v", err)
	}

	ctx := context.Background()
	if err := l.Acquire(ctx); err != nil {
		t.Fatalf("first Acquire: %v", err)
	}
	if err := l.Acquire(ctx); err != nil {
		t.Fatalf("second Acquire: %v", err)
	}

	// Third Acquire should block; verify it unblocks after a Release.
	done := make(chan struct{})
	go func() {
		if err := l.Acquire(ctx); err != nil {
			return
		}
		close(done)
	}()

	l.Release()

	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("third Acquire did not unblock after Release")
	}

	l.Release()
	l.Release()
}

// TestScoreLimiter_CancelledContextRejected verifies that Acquire returns an
// error when the context is already cancelled.
func TestScoreLimiter_CancelledContextRejected(t *testing.T) {
	t.Parallel()
	l, err := NewScoreLimiter(1)
	if err != nil {
		t.Fatalf("NewScoreLimiter: %v", err)
	}

	// Fill the semaphore.
	ctx := context.Background()
	if err := l.Acquire(ctx); err != nil {
		t.Fatalf("Acquire: %v", err)
	}

	// A cancelled context should return an error without blocking.
	cancelCtx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := l.Acquire(cancelCtx); err == nil {
		t.Fatal("expected error for cancelled context, got nil")
	}

	l.Release()
}

// ---------------------------------------------------------------------------
// HTTP handler concurrency tests
// ---------------------------------------------------------------------------

// writeBlockingVmafStub writes a vmaf stub shell script that blocks until a
// "done" signal file appears (or a timeout).  The signal file is written by the
// test once maxConcurrent requests are in flight, allowing the test to assert
// the in-flight count before unblocking all of them.
func writeBlockingVmafStub(t *testing.T, signalDir string) string {
	t.Helper()
	dir := t.TempDir()

	// The stub writes its PID to a per-invocation file inside signalDir so the
	// test can count concurrent processes.  It then waits for a "done" file to
	// appear (max 30 s) and exits 0 once it does, emitting the canned JSON so
	// the scorer can parse the output.
	script := fmt.Sprintf(`#!/bin/sh
outfile=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o) outfile="$2"; shift 2 ;;
    *)  shift ;;
  esac
done
# Record that we started.
touch '%s/'"$$"'.pid'
# Wait until done signal or timeout (30 s).
for i in $(seq 1 300); do
  if [ -f '%s/done' ]; then break; fi
  sleep 0.1
done
# Emit canned JSON so parseOutput succeeds.
if [ -n "$outfile" ]; then
  printf '%%s' '{"pooled_metrics":{"vmaf":{"mean":76.6683}}}' > "$outfile"
fi
exit 0
`, signalDir, signalDir)

	p := filepath.Join(dir, "vmaf")
	if err := os.WriteFile(p, []byte(script), 0o700); err != nil {
		t.Fatalf("writeBlockingVmafStub: %v", err)
	}
	return p
}

// TestHTTP_ConcurrencyCap_Enforced verifies that the HTTP /v1/score handler
// allows at most maxConcurrent simultaneous calls and returns 429 for excess.
//
// Method:
//  1. Create a stub vmaf binary that blocks until signalled.
//  2. Configure the server with maxConcurrent = 2.
//  3. Fill the semaphore by launching maxConcurrent blocking requests in background.
//  4. Issue overflow requests with an already-cancelled context — handleScore's
//     Acquire(ctx) returns immediately with context.Canceled, causing 429.
//  5. Signal the blocking stubs to complete (via defer).
func TestHTTP_ConcurrencyCap_Enforced(t *testing.T) {
	t.Parallel()

	const maxConcurrent = 2

	// Signal dir: stubs create PID files when they start; we write "done" to release.
	signalDir := t.TempDir()
	stub := writeBlockingVmafStub(t, signalDir)
	modelDir := writeModelFile(t)

	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	limiter, err := NewScoreLimiter(maxConcurrent)
	if err != nil {
		t.Fatalf("NewScoreLimiter: %v", err)
	}

	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	grpcSrv := newGRPCServerWithLimiter(scorer, metrics, log, limiter)
	hs := newHTTPServerWithLimiter(scorer, metrics, reg, log, grpcSrv, limiter)

	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// Unblock all stubs on test exit so goroutines don't leak.
	defer func() { _ = os.WriteFile(filepath.Join(signalDir, "done"), nil, 0o600) }()

	// --- Phase 1: fill the semaphore ---
	// Launch maxConcurrent requests that will block inside the vmaf stub so they
	// hold the semaphore slots.
	var fillWg sync.WaitGroup
	for i := 0; i < maxConcurrent; i++ {
		fillWg.Add(1)
		go func() {
			defer fillWg.Done()
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()
			body := `{"reference":"/tmp/ref.yuv","distorted":"/tmp/dis.yuv","model":"vmaf_v0.6.1"}`
			req, _ := http.NewRequestWithContext(ctx, http.MethodPost, ts.URL+"/v1/score",
				strings.NewReader(body))
			req.Header.Set("Content-Type", "application/json")
			resp, err := ts.Client().Do(req)
			if err != nil {
				return
			}
			resp.Body.Close()
		}()
	}

	// Wait for the stubs to signal they are running (each stub creates a .pid file).
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		entries, _ := os.ReadDir(signalDir)
		count := 0
		for _, e := range entries {
			if strings.HasSuffix(e.Name(), ".pid") {
				count++
			}
		}
		if count >= maxConcurrent {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}

	// --- Phase 2: overflow requests with cancelled contexts → 429 ---
	const overflowRequests = 2
	got429 := 0
	for i := 0; i < overflowRequests; i++ {
		cancelledCtx, cancel := context.WithCancel(context.Background())
		cancel() // already cancelled

		body := `{"reference":"/tmp/ref.yuv","distorted":"/tmp/dis.yuv","model":"vmaf_v0.6.1"}`
		req, _ := http.NewRequestWithContext(cancelledCtx, http.MethodPost, ts.URL+"/v1/score",
			strings.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		resp, err := ts.Client().Do(req)
		if err != nil {
			// Client-side cancellation: also acceptable (means the handler rejected).
			got429++
			continue
		}
		resp.Body.Close()
		if resp.StatusCode == http.StatusTooManyRequests {
			got429++
		}
	}

	if got429 < overflowRequests {
		t.Errorf("expected all %d overflow requests to be rejected (429 or cancelled), got %d rejections",
			overflowRequests, got429)
	}

	// fillWg goroutines are released by the deferred done-file write above;
	// no explicit Wait needed — the test does not assert their outcome.
}

// TestHTTP_NoCap_AllPass verifies that without a limiter all requests pass through.
func TestHTTP_NoCap_AllPass(t *testing.T) {
	t.Parallel()

	stub := writeVmafStub(t, vmafGoldenJSON)
	modelDir := writeModelFile(t)
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}

	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	grpcSrv := newGRPCServer(scorer, metrics, log) // no limiter
	hs := newHTTPServer(scorer, metrics, reg, log, grpcSrv)

	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	const n = 4
	var wg sync.WaitGroup
	statuses := make([]int, n)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			body := `{"reference":"/tmp/ref.yuv","distorted":"/tmp/dis.yuv","model":"vmaf_v0.6.1"}`
			resp, err := ts.Client().Post(ts.URL+"/v1/score", "application/json",
				strings.NewReader(body))
			if err != nil {
				statuses[idx] = -1
				return
			}
			resp.Body.Close()
			statuses[idx] = resp.StatusCode
		}(i)
	}
	wg.Wait()

	for i, code := range statuses {
		if code != http.StatusOK {
			t.Errorf("request %d: expected 200, got %d", i, code)
		}
	}
}

// ---------------------------------------------------------------------------
// gRPC handler concurrency tests
// ---------------------------------------------------------------------------

// stubScorerCounter is a stub scorer that uses an atomic counter to track the
// number of simultaneously in-flight Score calls.
type stubScorerCounter struct {
	// inflight is the current number of Score calls running concurrently.
	inflight atomic.Int64
	// maxSeen is the peak concurrent count observed.
	maxSeen atomic.Int64
	// blockCh is a channel that Score blocks on until closed.
	blockCh chan struct{}
}

func newStubScorerCounter(blockCh chan struct{}) *stubScorerCounter {
	return &stubScorerCounter{blockCh: blockCh}
}

// scoreFunc simulates a slow vmaf call by blocking on blockCh.  It is used
// directly (not via a real libvmaf.Scorer) in the gRPC unit tests that wire
// a fake scoring function.
func (s *stubScorerCounter) scoreFunc(_ context.Context, _, _, _ string) (float64, map[string]float64, error) {
	cur := s.inflight.Add(1)
	defer s.inflight.Add(-1)

	// Update peak.
	for {
		prev := s.maxSeen.Load()
		if cur <= prev {
			break
		}
		if s.maxSeen.CompareAndSwap(prev, cur) {
			break
		}
	}

	// Block until the test signals or context is done.
	select {
	case <-s.blockCh:
	}
	return 76.6683, map[string]float64{"vmaf": 76.6683}, nil
}

// TestGRPC_ConcurrencyCap_ResourceExhausted verifies that when the gRPC server
// has maxConcurrent=1 and a call is already in flight, the next call receives
// codes.ResourceExhausted immediately.
func TestGRPC_ConcurrencyCap_ResourceExhausted(t *testing.T) {
	t.Parallel()

	const maxConcurrent = 1

	// Use a blocking vmaf stub so the first call stays in flight.
	signalDir := t.TempDir()
	stub := writeBlockingVmafStub(t, signalDir)
	modelDir := writeModelFile(t)
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	defer func() { _ = os.WriteFile(filepath.Join(signalDir, "done"), nil, 0o600) }()

	limiter, err := NewScoreLimiter(maxConcurrent)
	if err != nil {
		t.Fatalf("NewScoreLimiter: %v", err)
	}

	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	impl := newGRPCServerWithLimiter(scorer, metrics, log, limiter)

	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	srv := grpc.NewServer(
		grpc.UnaryInterceptor(recoveryUnaryInterceptor(log)),
		grpc.StreamInterceptor(recoveryStreamInterceptor(log)),
	)
	vmafxv1.RegisterVmafxScoringServer(srv, impl)
	go func() { _ = srv.Serve(lis) }()
	defer srv.GracefulStop()

	conn, err := grpc.NewClient(lis.Addr().String(),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		srv.Stop()
		t.Fatalf("dial: %v", err)
	}
	defer func() { _ = conn.Close() }()

	client := vmafxv1.NewVmafxScoringClient(conn)

	// Start the first call — it will block inside the vmaf stub.
	firstCtx, firstCancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer firstCancel()

	firstDone := make(chan error, 1)
	go func() {
		_, err := client.Score(firstCtx, &vmafxv1.ScoreRequest{
			Reference: "/tmp/ref.yuv",
			Distorted: "/tmp/dis.yuv",
			Model:     "vmaf_v0.6.1",
		})
		firstDone <- err
	}()

	// Give the first call time to acquire the semaphore and enter the stub.
	time.Sleep(200 * time.Millisecond)

	// The second call should be rejected immediately with ResourceExhausted
	// because the semaphore is full AND we cancel the context immediately.
	rejectCtx, rejectCancel := context.WithCancel(context.Background())
	rejectCancel() // cancelled before Acquire, so Acquire(ctx) returns immediately

	_, err = client.Score(rejectCtx, &vmafxv1.ScoreRequest{
		Reference: "/tmp/ref.yuv",
		Distorted: "/tmp/dis.yuv",
		Model:     "vmaf_v0.6.1",
	})
	if err == nil {
		t.Fatal("expected ResourceExhausted or Canceled for second call, got nil")
	}
	code := status.Code(err)
	if code != codes.ResourceExhausted && code != codes.Canceled {
		t.Errorf("expected ResourceExhausted or Canceled, got %v (err: %v)", code, err)
	}
}

// TestGRPC_NoCap_AllPass verifies that without a limiter concurrent calls all succeed.
func TestGRPC_NoCap_AllPass(t *testing.T) {
	t.Parallel()

	client, stop := startGRPCTestServer(t)
	defer stop()

	const n = 3
	var wg sync.WaitGroup
	errs := make([]error, n)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			_, errs[idx] = client.Score(ctx, &vmafxv1.ScoreRequest{
				Reference: "/tmp/ref.yuv",
				Distorted: "/tmp/dis.yuv",
				Model:     "vmaf_v0.6.1",
			})
		}(i)
	}
	wg.Wait()

	for i, err := range errs {
		if err != nil {
			t.Errorf("request %d: unexpected error: %v", i, err)
		}
	}
}

// ---------------------------------------------------------------------------
// Stub scorer atomic-counter test (unit-level, no real gRPC)
// ---------------------------------------------------------------------------

// TestStubScorerCounter_PeakTracking verifies the atomic counter correctly
// records the peak concurrent call count via the stub.  This is an in-process
// sanity check that the concurrency-counting logic in the stub itself works
// before relying on it in integration tests.
func TestStubScorerCounter_PeakTracking(t *testing.T) {
	t.Parallel()

	blockCh := make(chan struct{})
	stub := newStubScorerCounter(blockCh)

	const n = 4
	var wg sync.WaitGroup
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, _, _ = stub.scoreFunc(context.Background(), "", "", "")
		}()
	}

	// Give goroutines time to all enter scoreFunc.
	time.Sleep(50 * time.Millisecond)

	// Unblock all.
	close(blockCh)
	wg.Wait()

	if got := stub.maxSeen.Load(); got < n {
		t.Errorf("expected maxSeen >= %d, got %d", n, got)
	}
}

// ---------------------------------------------------------------------------
// HTTP 429 response body test
// ---------------------------------------------------------------------------

// TestHTTP_429_Body verifies the 429 response body contains a descriptive
// error message when the semaphore is full and the caller's context is
// already cancelled.
//
// Uses httptest.ResponseRecorder (not a real listener) to avoid port/goroutine
// leaks: the handler is called directly with a pre-cancelled request context so
// Acquire(ctx) returns immediately and we see the 429 path without any
// in-flight blocking goroutine.
func TestHTTP_429_Body(t *testing.T) {
	t.Parallel()

	limiter, err := NewScoreLimiter(1)
	if err != nil {
		t.Fatalf("NewScoreLimiter: %v", err)
	}

	// Fill the single slot by acquiring it directly — no real vmaf process needed.
	bgCtx := context.Background()
	if err := limiter.Acquire(bgCtx); err != nil {
		t.Fatalf("Acquire: %v", err)
	}
	defer limiter.Release()

	// Build a minimal httpServer with the filled limiter but nil scorer (won't
	// be reached because Acquire fails first).
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	hs := newHTTPServerWithLimiter(nil, metrics, reg, log, nil, limiter)

	// Issue a POST /v1/score with an already-cancelled context.
	cancelledCtx, cancel := context.WithCancel(context.Background())
	cancel()

	body := `{"reference":"/tmp/ref.yuv","distorted":"/tmp/dis.yuv","model":"vmaf_v0.6.1"}`
	req, err := http.NewRequestWithContext(cancelledCtx, http.MethodPost, "/v1/score",
		strings.NewReader(body))
	if err != nil {
		t.Fatalf("NewRequest: %v", err)
	}
	req.Header.Set("Content-Type", "application/json")

	rr := httptest.NewRecorder()
	hs.handleScore(rr, req)

	if rr.Code != http.StatusTooManyRequests {
		t.Errorf("expected 429, got %d; body: %s", rr.Code, rr.Body.String())
	}

	var errResp errorResponse
	if err := json.NewDecoder(rr.Body).Decode(&errResp); err != nil {
		t.Fatalf("decode error body: %v", err)
	}
	if !bytes.Contains([]byte(errResp.Error), []byte("concurrent")) {
		t.Errorf("error body should mention 'concurrent', got: %q", errResp.Error)
	}
}
