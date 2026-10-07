// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/controller_fake_test.go — an in-process VmafxController
// served on a loopback listener, for the controller-client tests.
//
// The fake keeps the session semantics of the real controller: RegisterNode
// issues node-N / tok-N, PullWork and ReportResult refuse any token but the
// newest with PermissionDenied, and Heartbeat answers ok=false for it. Each
// RPC can be overridden per call number through the on* hooks.

package main

import (
	"context"
	"fmt"
	"log/slog"
	"maps"
	"net"
	"sync"
	"testing"
	"time"

	googlegrpc "google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

type fakeController struct {
	controllerv1.UnimplementedVmafxControllerServer

	mu       sync.Mutex
	sessions int
	calls    map[string]int
	auth     []string

	onRegister  func(call int) error
	onHeartbeat func(call int) bool
	onPull      func(call int, token string) (*controllerv1.Job, error)
	onReport    func(call int) error
	reports     chan *controllerv1.ReportResultRequest
	// cancelRunning, when set, answers an accepted heartbeat's
	// running_job_ids with the cancel_job_ids to send back.
	cancelRunning func(running []string) []string
	running       [][]string
}

func newFakeController() *fakeController {
	return &fakeController{calls: map[string]int{}, reports: make(chan *controllerv1.ReportResultRequest, 16)}
}

// count records one call of rpc and returns its 1-based number.
func (f *fakeController) count(rpc string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls[rpc]++
	return f.calls[rpc]
}

func (f *fakeController) callCount(rpc string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls[rpc]
}

func (f *fakeController) callsSnapshot() map[string]int {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := make(map[string]int, len(f.calls))
	maps.Copy(out, f.calls)
	return out
}

func (f *fakeController) currentToken() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return fmt.Sprintf("tok-%d", f.sessions)
}

func (f *fakeController) RegisterNode(_ context.Context, req *controllerv1.RegisterNodeRequest) (*controllerv1.RegisterNodeResponse, error) {
	call := f.count("RegisterNode")
	if f.onRegister != nil {
		if err := f.onRegister(call); err != nil {
			return nil, err
		}
	}
	if req.GetName() == "" {
		return nil, status.Error(codes.InvalidArgument, "node name is required")
	}
	f.mu.Lock()
	f.sessions++
	n := f.sessions
	f.mu.Unlock()
	return &controllerv1.RegisterNodeResponse{NodeId: fmt.Sprintf("node-%d", n), SessionToken: fmt.Sprintf("tok-%d", n)}, nil
}

func (f *fakeController) Heartbeat(_ context.Context, req *controllerv1.HeartbeatRequest) (*controllerv1.HeartbeatResponse, error) {
	call := f.count("Heartbeat")
	f.mu.Lock()
	f.running = append(f.running, req.GetRunningJobIds())
	f.mu.Unlock()
	ok := req.GetSessionToken() == f.currentToken()
	if ok && f.onHeartbeat != nil {
		ok = f.onHeartbeat(call)
	}
	resp := &controllerv1.HeartbeatResponse{Ok: ok}
	if ok && f.cancelRunning != nil {
		resp.CancelJobIds = f.cancelRunning(req.GetRunningJobIds())
	}
	return resp, nil
}

func (f *fakeController) PullWork(_ context.Context, req *controllerv1.PullWorkRequest) (*controllerv1.PullWorkResponse, error) {
	call := f.count("PullWork")
	if req.GetSessionToken() != f.currentToken() {
		return nil, status.Error(codes.PermissionDenied, "unknown or expired session")
	}
	if f.onPull == nil {
		return &controllerv1.PullWorkResponse{}, nil
	}
	job, err := f.onPull(call, req.GetSessionToken())
	if err != nil {
		return nil, err
	}
	return &controllerv1.PullWorkResponse{Job: job}, nil
}

func (f *fakeController) ReportResult(_ context.Context, req *controllerv1.ReportResultRequest) (*controllerv1.ReportResultResponse, error) {
	call := f.count("ReportResult")
	if req.GetSessionToken() != f.currentToken() {
		return nil, status.Error(codes.PermissionDenied, "invalid session")
	}
	if f.onReport != nil {
		if err := f.onReport(call); err != nil {
			return nil, err
		}
	}
	f.reports <- req
	return &controllerv1.ReportResultResponse{Ok: true}, nil
}

// recordAuth is a unary interceptor that keeps every authorization header.
func (f *fakeController) recordAuth(ctx context.Context, req any, _ *googlegrpc.UnaryServerInfo,
	handler googlegrpc.UnaryHandler,
) (any, error) {
	md, _ := metadata.FromIncomingContext(ctx)
	f.mu.Lock()
	f.auth = append(f.auth, md.Get("authorization")...)
	f.mu.Unlock()
	return handler(ctx, req)
}

// runningSeen returns the running_job_ids of every heartbeat so far.
func (f *fakeController) runningSeen() [][]string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([][]string(nil), f.running...)
}

func (f *fakeController) authHeaders() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]string(nil), f.auth...)
}

// serveFake serves f on a loopback port and returns its address.
func serveFake(t *testing.T, f *fakeController) string {
	t.Helper()
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	srv := googlegrpc.NewServer(googlegrpc.UnaryInterceptor(f.recordAuth))
	controllerv1.RegisterVmafxControllerServer(srv, f)
	go func() { _ = srv.Serve(lis) }()
	t.Cleanup(srv.Stop)
	return lis.Addr().String()
}

// fakeExecutor runs fn for every job.
type fakeExecutor struct {
	fn func(ctx context.Context, job *controllerv1.Job) ExecuteResult
}

func (e fakeExecutor) Execute(ctx context.Context, job *controllerv1.Job) ExecuteResult {
	return e.fn(ctx, job)
}

// testControllerConfig is a fast-cycling configuration for addr.
func testControllerConfig(addr string) controllerConfig {
	return controllerConfig{
		Addr: addr, RPCTimeout: 2 * time.Second, HeartbeatInterval: 50 * time.Millisecond,
		PollInterval: 20 * time.Millisecond, NodeName: "test-node", Slots: 1,
	}
}

// startTestClient dials addr in plaintext and starts a client around exec.
func startTestClient(t *testing.T, cfg controllerConfig, exec jobExecutor) *controllerClient {
	t.Helper()
	return startTestClientWith(t, cfg, exec, nil)
}

// startTestClientWith is startTestClient with the client recording into m.
func startTestClientWith(t *testing.T, cfg controllerConfig, exec jobExecutor, m *nodeMetrics) *controllerClient {
	t.Helper()
	conn, err := googlegrpc.NewClient(cfg.Addr, googlegrpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatalf("dial fake controller: %v", err)
	}
	capability, err := nodeCapability("cpu", cfg.Slots)
	if err != nil {
		t.Fatalf("nodeCapability: %v", err)
	}
	log := slog.New(slog.DiscardHandler)
	c := newControllerClient(cfg, controllerv1.NewVmafxControllerClient(conn), exec, capability, log)
	c.metrics = m
	c.start()
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		c.stop(ctx)
		_ = conn.Close()
	})
	return c
}

// awaitReport returns the next report or fails after timeout.
func awaitReport(t *testing.T, f *fakeController, timeout time.Duration) *controllerv1.ReportResultRequest {
	t.Helper()
	select {
	case r := <-f.reports:
		return r
	case <-time.After(timeout):
		t.Fatalf("no ReportResult within %v; calls=%v", timeout, f.callsSnapshot())
		return nil
	}
}

// oneJob hands out job exactly once, on the first PullWork that carries token
// (any token when token is empty).
func oneJob(job *controllerv1.Job, token string) func(int, string) (*controllerv1.Job, error) {
	var once sync.Once
	return func(_ int, got string) (*controllerv1.Job, error) {
		var out *controllerv1.Job
		if token == "" || got == token {
			once.Do(func() { out = job })
		}
		return out, nil
	}
}
