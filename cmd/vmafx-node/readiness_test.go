// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/readiness_test.go — readiness checks (/readyz) for the opt-in
// online-training sidecar and the gRPC listener (ADR-3558).

//go:build cgo

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
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

	"github.com/go-chi/chi/v5"
	"go.uber.org/fx"
	"go.uber.org/fx/fxtest"

	grpcmod "github.com/golusoris/golusoris/grpc"
	"github.com/golusoris/golusoris/observability/statuspage"

	"github.com/VMAFx/vmafx/cmd/vmafx-node/probe"
	"github.com/VMAFx/vmafx/internal/execstub"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

type verboseProbePayload struct {
	Status string              `json:"status"`
	Tag    string              `json:"tag"`
	Checks []statuspage.Result `json:"checks"`
}

func fetchRouterProbe(t *testing.T, h http.Handler, target string) (int, string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	req := httptest.NewRequestWithContext(ctx, http.MethodGet, target, nil)
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, req)
	return rr.Code, rr.Body.String()
}

func fetchLiveHTTP(t *testing.T, url string) (int, string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		t.Fatalf("build request %s: %v", url, err)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("GET %s: %v", url, err)
	}
	defer func() {
		if closeErr := resp.Body.Close(); closeErr != nil {
			t.Errorf("close response body: %v", closeErr)
		}
	}()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read %s: %v", url, err)
	}
	return resp.StatusCode, string(body)
}

func waitBounded(t *testing.T, desc string, timeout time.Duration, cond func() bool) {
	t.Helper()
	const poll = 20 * time.Millisecond
	steps := max(int(timeout/poll), 1)
	deadline := time.Now().Add(timeout)
	for range steps {
		if cond() {
			return
		}
		if !time.Now().Before(deadline) {
			break
		}
		time.Sleep(poll)
	}
	if !cond() {
		t.Fatalf("timed out after %v waiting for %s", timeout, desc)
	}
}

func buildTestScorer(t *testing.T) *libvmaf.Scorer {
	t.Helper()
	dir := t.TempDir()
	stub := filepath.Join(dir, "vmaf")
	execstub.Write(t, stub, []byte("#!/bin/sh\nexit 0\n"))
	modelDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(modelDir, "vmaf_v0.6.1.json"), []byte("{}"), 0o600); err != nil {
		t.Fatalf("write model fixture: %v", err)
	}
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	t.Cleanup(scorer.Close)
	return scorer
}

func startLocalTCPListener(t *testing.T) (string, func()) {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen tcp: %v", err)
	}
	var once sync.Once
	stop := func() {
		once.Do(func() {
			_ = ln.Close()
		})
	}
	t.Cleanup(stop)
	return ln.Addr().String(), stop
}

func startUnixSidecarStub(t *testing.T, sockPath string) func() {
	t.Helper()
	lis, err := net.Listen("unix", sockPath)
	if err != nil {
		t.Fatalf("listen unix %s: %v", sockPath, err)
	}
	var (
		mu    sync.Mutex
		conns []net.Conn
		once  sync.Once
	)
	go func() {
		for range 64 {
			conn, acceptErr := lis.Accept()
			if acceptErr != nil {
				return
			}
			mu.Lock()
			conns = append(conns, conn)
			mu.Unlock()
		}
	}()
	stop := func() {
		once.Do(func() {
			_ = lis.Close()
			mu.Lock()
			active := conns
			conns = nil
			mu.Unlock()
			for _, c := range active {
				_ = c.Close()
			}
		})
	}
	t.Cleanup(stop)
	return stop
}

func mountTestNodeRouter(t *testing.T, grpcAddr string, fc *FeedbackClient) (chi.Router, *statuspage.Registry) {
	t.Helper()
	router := chi.NewRouter()
	checks := statuspage.NewRegistry(nil)
	promReg, err := provideNodeRegistry()
	if err != nil {
		t.Fatalf("provideNodeRegistry: %v", err)
	}
	mountNodeHealth(nodeHealthParams{
		Registry: checks,
		Scorer:   buildTestScorer(t),
		Inv:      probe.EmptyInventory(),
		GRPC:     grpcmod.Config{Listen: grpcAddr},
		Sidecar:  fc,
		Log:      slog.New(slog.DiscardHandler),
	})
	mountNodeHTTP(router, promReg, checks)
	return router, checks
}

// TestSidecarReadiness_PlantedUnreachableNotReady is the planted case: when the
// sidecar is enabled and nothing listens on the socket, GET /readyz is not 200
// and the verbose probe body names sidecar along with the socket path and dial
// error, while /livez and /startupz stay 200.
func TestSidecarReadiness_PlantedUnreachableNotReady(t *testing.T) {
	sockPath := filepath.Join(t.TempDir(), "sidecar.sock")
	t.Setenv(feedbackSocketEnv, sockPath)

	fc := newFeedbackClient(slog.New(slog.DiscardHandler), true)
	fc.retryBase = 25 * time.Millisecond
	fc.retryMax = 100 * time.Millisecond
	fc.Start()
	t.Cleanup(fc.Close)

	grpcAddr, _ := startLocalTCPListener(t)
	router, _ := mountTestNodeRouter(t, grpcAddr, fc)

	waitBounded(t, "initial failed dial recorded", 2*time.Second, func() bool {
		return fc.lastErr.Load() != nil
	})

	code, _ := fetchRouterProbe(t, router, "/readyz")
	if code == http.StatusOK {
		t.Fatalf("GET /readyz = %d with unreachable sidecar, want non-200 (503)", code)
	}
	vCode, vBody := fetchRouterProbe(t, router, "/readyz?verbose=1")
	if vCode != http.StatusServiceUnavailable {
		t.Fatalf("GET /readyz?verbose=1 = %d, want 503: %s", vCode, vBody)
	}
	if !strings.Contains(vBody, `"sidecar"`) || !strings.Contains(vBody, sockPath) {
		t.Fatalf("verbose /readyz body lacks sidecar or socket path %s: %s", sockPath, vBody)
	}
	if liveCode, _ := fetchRouterProbe(t, router, "/livez"); liveCode != http.StatusOK {
		t.Fatalf("GET /livez = %d, want 200 (sidecar must not tag liveness)", liveCode)
	}
	if startCode, _ := fetchRouterProbe(t, router, "/startupz"); startCode != http.StatusOK {
		t.Fatalf("GET /startupz = %d, want 200 (sidecar must not tag startup)", startCode)
	}
}

// TestSidecarReadiness_ReadyWhenListenerAccepts verifies that /readyz recovers
// to 200 within a bounded deadline once a Unix socket listener starts accepting.
func TestSidecarReadiness_ReadyWhenListenerAccepts(t *testing.T) {
	sockPath := filepath.Join(t.TempDir(), "sidecar.sock")
	t.Setenv(feedbackSocketEnv, sockPath)

	fc := newFeedbackClient(slog.New(slog.DiscardHandler), true)
	fc.retryBase = 30 * time.Millisecond
	fc.retryMax = 100 * time.Millisecond
	fc.Start()
	t.Cleanup(fc.Close)

	grpcAddr, _ := startLocalTCPListener(t)
	router, _ := mountTestNodeRouter(t, grpcAddr, fc)

	if code, _ := fetchRouterProbe(t, router, "/readyz"); code == http.StatusOK {
		t.Fatalf("GET /readyz before listener start = %d, want non-200", code)
	}

	stopSidecar := startUnixSidecarStub(t, sockPath)
	defer stopSidecar()

	waitBounded(t, "/readyz 200 after sidecar listener starts", 3*time.Second, func() bool {
		code, _ := fetchRouterProbe(t, router, "/readyz")
		return code == http.StatusOK
	})
}

// TestSidecarReadiness_NotReadyAfterListenerCloses verifies that closing the
// sidecar listener after connection drops /readyz back to non-200 within a
// bounded deadline even while no feedback messages are queued.
func TestSidecarReadiness_NotReadyAfterListenerCloses(t *testing.T) {
	sockPath := filepath.Join(t.TempDir(), "sidecar.sock")
	t.Setenv(feedbackSocketEnv, sockPath)

	stopSidecar := startUnixSidecarStub(t, sockPath)
	fc := newFeedbackClient(slog.New(slog.DiscardHandler), true)
	fc.retryBase = 30 * time.Millisecond
	fc.retryMax = 100 * time.Millisecond
	fc.Start()
	t.Cleanup(fc.Close)

	grpcAddr, _ := startLocalTCPListener(t)
	router, _ := mountTestNodeRouter(t, grpcAddr, fc)

	waitBounded(t, "/readyz 200 while listener open", 3*time.Second, func() bool {
		code, _ := fetchRouterProbe(t, router, "/readyz")
		return code == http.StatusOK
	})

	stopSidecar()

	waitBounded(t, "/readyz non-200 after listener closes", 3*time.Second, func() bool {
		code, _ := fetchRouterProbe(t, router, "/readyz")
		return code != http.StatusOK
	})
	_, vBody := fetchRouterProbe(t, router, "/readyz?verbose=1")
	if !strings.Contains(vBody, `"sidecar"`) {
		t.Fatalf("verbose /readyz after close lacks sidecar check: %s", vBody)
	}
}

// TestSidecarReadiness_DisabledRegistersNoCheckAndZeroDials verifies that with
// sidecar.enabled unset/false no sidecar check is registered, /readyz returns
// 200, Send counts drops, startup logs "sidecar disabled", and zero dials run.
func TestSidecarReadiness_DisabledRegistersNoCheckAndZeroDials(t *testing.T) {
	var logBuf bytes.Buffer
	logger := slog.New(slog.NewTextHandler(&logBuf, &slog.HandlerOptions{Level: slog.LevelInfo}))
	cfg := newTestConfig(t, map[string]string{
		"VMAFX_SIDECAR_SOCKET": filepath.Join(t.TempDir(), "unused.sock"),
	})

	var fc *FeedbackClient
	var dialCount atomic.Int64
	app := fxtest.New(t,
		fx.NopLogger,
		fx.Supply(cfg, logger),
		fx.Provide(provideFeedbackClient),
		fx.Decorate(func(c *FeedbackClient) *FeedbackClient {
			c.dialFn = func(context.Context, string) (net.Conn, error) {
				dialCount.Add(1)
				return nil, net.ErrClosed
			}
			return c
		}),
		fx.Populate(&fc),
	)
	app.RequireStart()
	t.Cleanup(app.RequireStop)

	grpcAddr, _ := startLocalTCPListener(t)
	router, checks := mountTestNodeRouter(t, grpcAddr, fc)

	if code, _ := fetchRouterProbe(t, router, "/readyz"); code != http.StatusOK {
		t.Fatalf("GET /readyz with sidecar disabled = %d, want 200", code)
	}
	for _, res := range checks.Run(context.Background()) {
		if res.Name == "sidecar" {
			t.Fatalf("disabled sidecar registered check %+v", res)
		}
	}
	if fc.Send(&FeedbackMessage{JobID: "dropped-when-disabled"}) || fc.Dropped() != 1 {
		t.Fatalf("Send on disabled client: dropped=%d, want 1", fc.Dropped())
	}
	if got := dialCount.Load(); got != 0 {
		t.Fatalf("dial count with sidecar disabled = %d, want 0", got)
	}
	if !strings.Contains(logBuf.String(), "sidecar disabled") {
		t.Fatalf("startup log %q does not contain 'sidecar disabled'", logBuf.String())
	}
}

// TestSidecarEnabled_InvalidValueFailsConstruction verifies an invalid
// VMAFX_SIDECAR_ENABLED value fails graph construction naming sidecar.enabled.
func TestSidecarEnabled_InvalidValueFailsConstruction(t *testing.T) {
	writeNodeEnv(t)
	t.Setenv("VMAFX_SIDECAR_ENABLED", "notabool")

	err := fx.New(productionGraph(), fx.NopLogger).Err()
	if err == nil || !strings.Contains(err.Error(), "sidecar.enabled") {
		t.Fatalf("graph error = %v, want startup failure naming sidecar.enabled", err)
	}
}

// TestSidecarProductionGraph_EndToEndReadiness exercises VMAFX_SIDECAR_ENABLED
// through the real production fx graph and HTTP listener.
func TestSidecarProductionGraph_EndToEndReadiness(t *testing.T) {
	sockPath := filepath.Join(t.TempDir(), "sidecar.sock")
	app := startNodeAppWithRetry(t, func() {
		writeDialableNodeEnv(t)
		t.Setenv("VMAFX_SIDECAR_ENABLED", "true")
		t.Setenv("VMAFX_SIDECAR_SOCKET", sockPath)
	}, productionGraph())
	defer app.RequireStop()

	base := "http://" + os.Getenv("VMAFX_HTTP_ADDR")
	code, body := fetchLiveHTTP(t, base+"/readyz?verbose=1")
	if code != http.StatusServiceUnavailable || !strings.Contains(body, `"sidecar"`) {
		t.Fatalf("GET /readyz?verbose=1 = %d (%s), want 503 naming sidecar", code, body)
	}

	stopSidecar := startUnixSidecarStub(t, sockPath)
	defer stopSidecar()

	waitBounded(t, "production /readyz 200 after sidecar starts", 6*time.Second, func() bool {
		c, _ := fetchLiveHTTP(t, base+"/readyz")
		return c == http.StatusOK
	})
}

// TestGRPCReadinessCheck_AcceptingAndClosed verifies the statuspage grpc check
// passes while the gRPC port accepts connections and fails when closed.
func TestGRPCReadinessCheck_AcceptingAndClosed(t *testing.T) {
	grpcAddr, stopListener := startLocalTCPListener(t)
	fc := newFeedbackClient(slog.New(slog.DiscardHandler), false)
	router, _ := mountTestNodeRouter(t, grpcAddr, fc)

	if code, _ := fetchRouterProbe(t, router, "/readyz"); code != http.StatusOK {
		t.Fatalf("GET /readyz with open gRPC listener = %d, want 200", code)
	}

	stopListener()

	code, body := fetchRouterProbe(t, router, "/readyz?verbose=1")
	if code != http.StatusServiceUnavailable {
		t.Fatalf("GET /readyz?verbose=1 with closed gRPC listener = %d, want 503", code)
	}
	var payload verboseProbePayload
	if err := json.Unmarshal([]byte(body), &payload); err != nil {
		t.Fatalf("decode verbose /readyz: %v", err)
	}
	foundDown := false
	for _, chk := range payload.Checks {
		if chk.Name == "grpc" && chk.Status == statuspage.StatusDown {
			foundDown = true
		}
	}
	if !foundDown {
		t.Fatalf("expected grpc check down in %+v", payload.Checks)
	}
}

func TestGRPCLoopbackAddr(t *testing.T) {
	t.Parallel()
	cases := map[string]string{
		":50052":          "127.0.0.1:50052",
		"0.0.0.0:50052":   "127.0.0.1:50052",
		"[::]:50052":      "[::1]:50052",
		"127.0.0.1:50052": "127.0.0.1:50052",
	}
	for input, want := range cases {
		got, err := grpcLoopbackAddr(input)
		if err != nil || got != want {
			t.Errorf("grpcLoopbackAddr(%q) = %q, %v; want %q, nil", input, got, err, want)
		}
	}
	for _, invalid := range []string{"bad-addr", "127.0.0.1:0", ":0"} {
		if got, err := grpcLoopbackAddr(invalid); err == nil {
			t.Errorf("grpcLoopbackAddr(%q) = %q, nil; want error", invalid, got)
		}
	}
}
