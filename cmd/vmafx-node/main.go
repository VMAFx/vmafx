// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/main.go — vmafx-node worker binary entry point.
//
// vmafx-node is a stateless worker that:
//  1. Detects available GPU hardware.
//  2. Registers with vmafx-controller via gRPC RegisterNode.
//  3. Runs a heartbeat goroutine (every 10 s).
//  4. Polls PullWork in a loop; for each job calls Execute then ReportResult.
//  5. On SIGTERM: finishes the current job, sends Deregister (Heartbeat with
//     0 running + explicit exit flag), and exits cleanly within 30 s.
//
// Configuration is 12-factor env-only:
//
//	VMAFX_CONTROLLER_ADDR  — controller gRPC address (host:port)
//	VMAFX_NODE_ID          — override computed node ID (defaults to hostname)
//	VMAFX_BACKEND          — force backend (cpu/cuda/sycl/hip/metal); auto-detected if unset
//	VMAFX_GPU_DEVICE       — GPU device index (default 0)
//	VMAFX_LOG_LEVEL        — debug/info/warn/error (default info)
//	VMAFX_MODEL_DIR        — path to ONNX model directory
//
// Prometheus metrics are exposed on :9090/metrics.
//
// ADR-0709: VMAFX Phase 4b distributed platform.
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
// ADR-0713: vmafx-node Go worker binary.

package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/ai"
	"github.com/VMAFx/vmafx/pkg/encoder"
	"github.com/VMAFx/vmafx/pkg/gpu"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// gracefulShutdownTimeout is the maximum time we wait for the current job to
// finish after SIGTERM before forcing exit.
const gracefulShutdownTimeout = 30 * time.Second

// heartbeatInterval is how often the node sends a Heartbeat RPC.
const heartbeatInterval = 10 * time.Second

// pullPollInterval is how long the node waits between PullWork calls when
// the queue is empty.
const pullPollInterval = 2 * time.Second

// Prometheus metrics.
var (
	jobsTotal = promauto.NewCounterVec(prometheus.CounterOpts{
		Namespace: "vmafx",
		Subsystem: "node",
		Name:      "jobs_total",
		Help:      "Total jobs processed by outcome (success, failure, unsupported).",
	}, []string{"outcome"})

	jobDurationSeconds = promauto.NewHistogram(prometheus.HistogramOpts{
		Namespace: "vmafx",
		Subsystem: "node",
		Name:      "job_duration_seconds",
		Help:      "Wall-clock duration of job execution in seconds.",
		Buckets:   prometheus.ExponentialBuckets(0.1, 2, 10),
	})

	heartbeatErrors = promauto.NewCounter(prometheus.CounterOpts{
		Namespace: "vmafx",
		Subsystem: "node",
		Name:      "heartbeat_errors_total",
		Help:      "Number of failed Heartbeat RPCs.",
	})
)

// config holds the resolved 12-factor configuration for the node.
type config struct {
	controllerAddr string
	nodeID         string
	backend        string
	gpuDevice      string
	logLevel       slog.Level
	modelDir       string
	metricsAddr    string
}

// loadConfig reads environment variables and fills a config struct.
func loadConfig() (config, error) {
	addr := os.Getenv("VMAFX_CONTROLLER_ADDR")
	if addr == "" {
		return config{}, fmt.Errorf("VMAFX_CONTROLLER_ADDR is required")
	}

	nodeID := os.Getenv("VMAFX_NODE_ID")
	if nodeID == "" {
		h, err := os.Hostname()
		if err != nil {
			return config{}, fmt.Errorf("cannot determine node ID: %w", err)
		}
		nodeID = h
	}

	logLevel := slog.LevelInfo
	if ll := os.Getenv("VMAFX_LOG_LEVEL"); ll != "" {
		if err := logLevel.UnmarshalText([]byte(ll)); err != nil {
			return config{}, fmt.Errorf("invalid VMAFX_LOG_LEVEL %q: %w", ll, err)
		}
	}

	return config{
		controllerAddr: addr,
		nodeID:         nodeID,
		backend:        os.Getenv("VMAFX_BACKEND"),
		gpuDevice:      os.Getenv("VMAFX_GPU_DEVICE"),
		logLevel:       logLevel,
		modelDir:       os.Getenv("VMAFX_MODEL_DIR"),
		metricsAddr:    ":9090",
	}, nil
}

// node holds all the runtime state for a vmafx-node instance.
type node struct {
	cfg          config
	log          *slog.Logger
	gpuCap       gpu.Capability
	controllerCC *grpc.ClientConn
	client       controllerv1.VmafxControllerClient
	executor     *Executor
	sessionToken string
	jobsRunning  atomic.Int32
}

func main() {
	cfg, err := loadConfig()
	if err != nil {
		//nolint:forbidigo // initial startup errors must go to stderr before slog is set up
		fmt.Fprintf(os.Stderr, "vmafx-node: config error: %v\n", err)
		os.Exit(1)
	}

	log := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{
		Level: cfg.logLevel,
	}))
	slog.SetDefault(log)

	n := &node{cfg: cfg, log: log}
	if err := n.run(); err != nil {
		log.Error("vmafx-node exited with error", slog.String("error", err.Error()))
		os.Exit(1)
	}
}

// run is the main lifecycle for the node.  It blocks until graceful shutdown.
func (n *node) run() error {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// 1. Detect GPU.
	n.gpuCap = gpu.Detect()
	if n.cfg.backend == "" {
		n.cfg.backend = n.gpuCap.PrimaryBackend()
	}
	n.log.Info("gpu detection complete",
		slog.String("vendor", string(n.gpuCap.Vendor)),
		slog.String("device", n.gpuCap.DeviceName),
		slog.String("backend", n.cfg.backend),
		slog.Int("device_count", n.gpuCap.DeviceCount),
	)

	// 2. Codec discovery.
	hwEncoders := encoder.AvailableHardwareEncoders()
	n.log.Info("encoder discovery",
		slog.Any("hardware_encoders", hwEncoders),
	)

	// 3. Connect to controller.
	cc, err := grpc.NewClient(
		n.cfg.controllerAddr,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		return fmt.Errorf("dial controller %s: %w", n.cfg.controllerAddr, err)
	}
	n.controllerCC = cc
	defer func() {
		if closeErr := cc.Close(); closeErr != nil {
			n.log.Warn("grpc close error", slog.String("error", closeErr.Error()))
		}
	}()
	n.client = controllerv1.NewVmafxControllerClient(cc)

	// 4. Initialise scorer and AI registry.
	scorer, scorerErr := libvmaf.New(libvmaf.FindBinary(), n.cfg.modelDir)
	if scorerErr != nil {
		n.log.Warn("libvmaf scorer init failed — scoring jobs will fail",
			slog.String("error", scorerErr.Error()))
	}
	aiReg := ai.NewRegistry(n.cfg.modelDir)
	n.executor = NewExecutor(scorer, aiReg, n.cfg.backend, n.log)

	// 5. Start Prometheus metrics server.
	metricsSrv := &http.Server{
		Addr:        n.cfg.metricsAddr,
		Handler:     promhttp.Handler(),
		ReadTimeout: 5 * time.Second,
	}
	go func() {
		n.log.Info("metrics server starting", slog.String("addr", n.cfg.metricsAddr))
		if serveErr := metricsSrv.ListenAndServe(); serveErr != nil && !errors.Is(serveErr, http.ErrServerClosed) {
			n.log.Warn("metrics server error", slog.String("error", serveErr.Error()))
		}
	}()

	// 6. Register with controller.
	if err := n.register(ctx); err != nil {
		return fmt.Errorf("register: %w", err)
	}

	// 7. Start heartbeat goroutine.
	hbDone := make(chan struct{})
	go func() {
		defer close(hbDone)
		n.heartbeatLoop(ctx)
	}()

	// 8. Signal handling.
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGTERM, syscall.SIGINT)

	// 9. Main work loop.
	workDone := make(chan struct{})
	go func() {
		defer close(workDone)
		n.workLoop(ctx)
	}()

	// Wait for a signal.
	sig := <-sigCh
	n.log.Info("shutdown signal received", slog.String("signal", sig.String()))

	// Cancel context to stop heartbeat + work loops.
	cancel()

	// Wait up to gracefulShutdownTimeout for work loop to drain.
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), gracefulShutdownTimeout)
	defer shutdownCancel()

	select {
	case <-workDone:
		n.log.Info("work loop drained cleanly")
	case <-shutdownCtx.Done():
		n.log.Warn("graceful shutdown timeout — forcing exit")
	}
	<-hbDone

	// Shutdown metrics server.
	metricsShutdownCtx, metricsCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer metricsCancel()
	if shutdownErr := metricsSrv.Shutdown(metricsShutdownCtx); shutdownErr != nil {
		n.log.Warn("metrics server shutdown error", slog.String("error", shutdownErr.Error()))
	}

	n.log.Info("vmafx-node shutdown complete")
	return nil
}

// register sends RegisterNode to the controller and stores the session token.
func (n *node) register(ctx context.Context) error {
	regCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()

	resp, err := n.client.RegisterNode(regCtx, &controllerv1.RegisterNodeRequest{
		Name: n.cfg.nodeID,
		Capability: &controllerv1.NodeCapability{
			GpuVendor:   string(n.gpuCap.Vendor),
			Backends:    n.gpuCap.Backends,
			Concurrency: 1, // Stage 1: one job at a time.
		},
	})
	if err != nil {
		return fmt.Errorf("RegisterNode RPC: %w", err)
	}
	n.cfg.nodeID = resp.GetNodeId()
	n.sessionToken = resp.GetSessionToken()
	n.log.Info("registered with controller",
		slog.String("node_id", n.cfg.nodeID),
	)
	return nil
}

// heartbeatLoop sends heartbeat RPCs every heartbeatInterval until ctx is done.
func (n *node) heartbeatLoop(ctx context.Context) {
	ticker := time.NewTicker(heartbeatInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			hbCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
			resp, err := n.client.Heartbeat(hbCtx, &controllerv1.HeartbeatRequest{
				NodeId:       n.cfg.nodeID,
				SessionToken: n.sessionToken,
				JobsRunning:  n.jobsRunning.Load(),
			})
			cancel()
			if err != nil {
				n.log.Warn("heartbeat error", slog.String("error", err.Error()))
				heartbeatErrors.Inc()
				continue
			}
			if !resp.GetOk() {
				// Controller no longer recognises this session; re-register.
				n.log.Warn("controller rejected heartbeat — re-registering")
				reRegCtx, reRegCancel := context.WithTimeout(ctx, 10*time.Second)
				if reRegErr := n.register(reRegCtx); reRegErr != nil {
					n.log.Error("re-register failed", slog.String("error", reRegErr.Error()))
				}
				reRegCancel()
			}
		}
	}
}

// workLoop repeatedly calls PullWork → Execute → ReportResult.
func (n *node) workLoop(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		pullCtx, pullCancel := context.WithTimeout(ctx, 5*time.Second)
		resp, err := n.client.PullWork(pullCtx, &controllerv1.PullWorkRequest{
			NodeId:       n.cfg.nodeID,
			SessionToken: n.sessionToken,
			Capability: &controllerv1.NodeCapability{
				GpuVendor:   string(n.gpuCap.Vendor),
				Backends:    n.gpuCap.Backends,
				Concurrency: 1,
			},
		})
		pullCancel()
		if err != nil {
			n.log.Warn("PullWork error", slog.String("error", err.Error()))
			select {
			case <-ctx.Done():
				return
			case <-time.After(pullPollInterval):
				continue
			}
		}

		job := resp.GetJob()
		if job == nil {
			// Queue empty — back off.
			select {
			case <-ctx.Done():
				return
			case <-time.After(pullPollInterval):
				continue
			}
		}

		n.executeAndReport(ctx, job)
	}
}

// executeAndReport runs a job and reports the result to the controller.
func (n *node) executeAndReport(ctx context.Context, job *controllerv1.Job) {
	n.jobsRunning.Add(1)
	defer n.jobsRunning.Add(-1)

	t0 := time.Now()
	result := n.executor.Execute(ctx, job)
	duration := time.Since(t0).Seconds()
	jobDurationSeconds.Observe(duration)

	req := &controllerv1.ReportResultRequest{
		NodeId:       n.cfg.nodeID,
		SessionToken: n.sessionToken,
		JobId:        job.GetId(),
		Final:        true,
	}

	if result.Error != nil {
		req.Error = result.Error.Error()
		jobsTotal.WithLabelValues("failure").Inc()
		n.log.Error("job failed",
			slog.String("job_id", job.GetId()),
			slog.String("error", result.Error.Error()),
			slog.Float64("duration_s", duration),
		)
	} else {
		req.Score = result.Score
		req.Features = result.Features
		jobsTotal.WithLabelValues("success").Inc()
		n.log.Info("job completed",
			slog.String("job_id", job.GetId()),
			slog.Float64("score", result.Score),
			slog.Float64("duration_s", duration),
		)
	}

	reportCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	if _, reportErr := n.client.ReportResult(reportCtx, req); reportErr != nil {
		n.log.Error("ReportResult failed",
			slog.String("job_id", job.GetId()),
			slog.String("error", reportErr.Error()),
		)
	}
}
