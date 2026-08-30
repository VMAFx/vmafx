// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/http_server.go — HTTP handlers for vmafx-server.
//
// Endpoints:
//   GET  /healthz          — liveness probe; always 200 while the process is live.
//   GET  /readyz           — readiness probe; 200 once vmaf binary is reachable.
//   GET  /metrics          — Prometheus exposition format.
//   POST /v1/score         — JSON scoring endpoint (parity with Python implementation).
//   GET  /v1/health        — OpenAPI-spec liveness probe (delegates to grpc.Health).
//   GET  /v1/ready         — OpenAPI-spec readiness probe.
//   GET  /swagger          — Swagger UI (CDN-hosted, spec served from /swagger/spec.json).
//   GET  /swagger/spec.json — Embedded OpenAPI spec JSON.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.
// ADR-0797: vmafx-server OpenAPI REST contract.

//go:build cgo

package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// maxScoreRequestBodyBytes caps the request body size for POST /v1/score.
// The endpoint only consumes a small JSON blob with three string fields
// (reference path, distorted path, model name); 1 MiB is well above any
// legitimate payload (the longest realistic path on Linux is PATH_MAX = 4096
// bytes per field) and well below the threshold where a single request can
// pressure the heap. Without this cap an unauthenticated POST with a multi-GB
// body would balloon the JSON decoder's read buffer until OOM. ADR-0978.
const maxScoreRequestBodyBytes = 1 << 20 // 1 MiB

// scoreRequest mirrors the /v1/score JSON body.
type scoreRequest struct {
	Reference string `json:"reference"`
	Distorted string `json:"distorted"`
	Model     string `json:"model,omitempty"`
}

// scoreResponse is the /v1/score JSON response body.
type scoreResponse struct {
	Score    float64            `json:"score"`
	Features map[string]float64 `json:"features"`
}

// errorResponse is used for HTTP error bodies.
type errorResponse struct {
	Error string `json:"error"`
}

// httpServer groups the HTTP handler state.
type httpServer struct {
	scorer   *libvmaf.Scorer
	metrics  *observability.Metrics
	log      *slog.Logger
	registry *prometheus.Registry
	// grpc is the shared gRPC service implementation used by the REST adapter
	// (ADR-0797) to avoid duplicating business logic.
	grpc *grpcServer
	// limiter caps concurrent in-flight Scorer.Score calls on the HTTP path.
	// Must be the same ScoreLimiter instance as grpcServer.limiter so the cap is
	// enforced system-wide across both protocols.  nil means uncapped (test
	// compatibility: existing tests that call newHTTPServer directly without a
	// limiter retain old behaviour; production always sets one via
	// newHTTPServerWithLimiter).
	limiter *ScoreLimiter
}

// newHTTPServer creates an httpServer without a concurrency limiter.
// Kept for test compatibility. Production code should use newHTTPServerWithLimiter.
func newHTTPServer(
	scorer *libvmaf.Scorer,
	metrics *observability.Metrics,
	registry *prometheus.Registry,
	log *slog.Logger,
	grpc *grpcServer,
) *httpServer {
	return &httpServer{
		scorer:   scorer,
		metrics:  metrics,
		log:      log,
		registry: registry,
		grpc:     grpc,
	}
}

// newHTTPServerWithLimiter creates an httpServer with the given concurrency
// limiter.  Pass the same ScoreLimiter instance used by grpcServer so the cap
// is enforced across both protocols.
func newHTTPServerWithLimiter(
	scorer *libvmaf.Scorer,
	metrics *observability.Metrics,
	registry *prometheus.Registry,
	log *slog.Logger,
	grpc *grpcServer,
	limiter *ScoreLimiter,
) *httpServer {
	return &httpServer{
		scorer:   scorer,
		metrics:  metrics,
		log:      log,
		registry: registry,
		grpc:     grpc,
		limiter:  limiter,
	}
}

// routes registers all HTTP handlers on mux.
//
// Legacy endpoints (/healthz, /readyz, /v1/score) are preserved for backwards
// compatibility.  The OpenAPI-spec endpoints (/v1/health, /v1/ready) are
// registered via the REST adapter (ADR-0797).  /swagger serves the Swagger UI.
func (h *httpServer) routes(mux *http.ServeMux) {
	// Legacy Kubernetes probe aliases — kept for backwards compatibility.
	mux.HandleFunc("/healthz", h.handleHealthz)
	mux.HandleFunc("/readyz", h.handleReadyz)

	// Prometheus metrics.
	mux.Handle("/metrics", promhttp.HandlerFor(h.registry, promhttp.HandlerOpts{}))

	// Legacy /v1/score — retained for clients predating the OpenAPI contract.
	mux.HandleFunc("/v1/score", h.handleScore)

	// OpenAPI REST adapter: /v1/health, /v1/ready, and the preferred
	// /v1/score path (oapi-codegen-generated routing via restAdapter).
	if h.grpc != nil {
		adapter := newRestAdapter(h.grpc, h.log)
		// Register /v1/health and /v1/ready via the generated handler.
		// Note: /v1/score is already registered above via the legacy handler;
		// the adapter's ScoreVideoPair validates via oapi types but delegates
		// identically so both paths are equivalent.
		mux.HandleFunc("GET /v1/health", adapter.GetHealth)
		mux.HandleFunc("GET /v1/ready", adapter.GetReady)
	}

	// Swagger UI at /swagger and /swagger/spec.json.
	registerSwaggerUI(mux, h.log)
}

// handleHealthz is the liveness probe — always returns 200 OK.
func (h *httpServer) handleHealthz(w http.ResponseWriter, r *http.Request) {
	h.metrics.HealthRequests.Inc()
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(`{"status":"ok"}`))
}

// handleReadyz is the readiness probe — returns 503 until the scorer is usable.
func (h *httpServer) handleReadyz(w http.ResponseWriter, r *http.Request) {
	h.metrics.ReadyRequests.Inc()
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	if h.scorer == nil {
		w.WriteHeader(http.StatusServiceUnavailable)
		_, _ = w.Write([]byte(`{"status":"not ready","reason":"scorer not initialised"}`))
		return
	}
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(`{"status":"ready"}`))
}

// handleScore handles POST /v1/score.
func (h *httpServer) handleScore(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	h.metrics.ScoreRequests.Inc()
	start := time.Now()

	// Cap the request body at maxScoreRequestBodyBytes. http.MaxBytesReader
	// closes the underlying body when the limit trips and surfaces the cause
	// to the decoder as `*http.MaxBytesError`, which we map to 413 below.
	r.Body = http.MaxBytesReader(w, r.Body, maxScoreRequestBodyBytes)

	var req scoreRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		h.metrics.ScoreErrors.Inc()
		var maxBytesErr *http.MaxBytesError
		if errors.As(err, &maxBytesErr) {
			writeJSON(w, http.StatusRequestEntityTooLarge, errorResponse{
				Error: fmt.Sprintf("request body exceeds %d bytes", maxScoreRequestBodyBytes),
			})
			return
		}
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: fmt.Sprintf("invalid JSON body: %v", err)})
		return
	}

	if req.Reference == "" || req.Distorted == "" {
		h.metrics.ScoreErrors.Inc()
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "reference and distorted are required"})
		return
	}

	// Enforce the concurrency cap before forking a vmaf subprocess.
	// We use a non-blocking TryAcquire pattern here: if the semaphore is full
	// we return 429 immediately so the client can back off. We do NOT use
	// Acquire(ctx) in the HTTP path because the HTTP write deadline would
	// expire before the client sees the response body.  The gRPC path uses
	// Acquire(ctx) because gRPC backpressure handles the wait correctly.
	if h.limiter != nil {
		// Use context-aware Acquire so a cancelled request does not park in
		// the queue and waste a slot after the client has gone away.
		if err := h.limiter.Acquire(r.Context()); err != nil {
			h.metrics.ScoreErrors.Inc()
			h.log.Warn("http Score rejected: concurrency cap reached",
				"max", h.limiter.Max(), "error", err)
			writeJSON(w, http.StatusTooManyRequests, errorResponse{
				Error: fmt.Sprintf("too many concurrent scoring requests (max %d); try again later", h.limiter.Max()),
			})
			return
		}
		defer h.limiter.Release()
	}

	// Pass the request-scoped context so a client disconnect (or the
	// server's read/write timeout) propagates SIGKILL to the vmaf
	// subprocess via exec.CommandContext.  Fixes
	// T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.
	score, features, err := h.scorer.Score(r.Context(), req.Reference, req.Distorted, req.Model)
	elapsed := time.Since(start).Seconds()
	h.metrics.ScoreDuration.Observe(elapsed)

	if err != nil {
		h.metrics.ScoreErrors.Inc()
		h.log.Error("http Score failed", "error", err, "duration_s", elapsed)
		writeJSON(w, http.StatusInternalServerError, errorResponse{Error: err.Error()})
		return
	}

	h.log.Info("http Score completed",
		"score", fmt.Sprintf("%.4f", score),
		"duration_s", elapsed,
	)
	writeJSON(w, http.StatusOK, scoreResponse{Score: score, Features: features})
}

// writeJSON serialises v as JSON and writes it with the given status code.
func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
}
