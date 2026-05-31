// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/http_server.go — HTTP handlers for vmafx-server.
//
// Endpoints:
//   GET  /healthz      — liveness probe; always 200 while the process is live.
//   GET  /readyz       — readiness probe; 200 once vmaf binary is reachable.
//   GET  /metrics      — Prometheus exposition format.
//   POST /v1/score     — JSON scoring endpoint (parity with Python implementation).
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.

//go:build cgo

package main

import (
	"context"
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
}

// newHTTPServer creates an httpServer.
func newHTTPServer(
	scorer *libvmaf.Scorer,
	metrics *observability.Metrics,
	registry *prometheus.Registry,
	log *slog.Logger,
) *httpServer {
	return &httpServer{
		scorer:   scorer,
		metrics:  metrics,
		log:      log,
		registry: registry,
	}
}

// routes registers all HTTP handlers on mux.
func (h *httpServer) routes(mux *http.ServeMux) {
	mux.HandleFunc("/healthz", h.handleHealthz)
	mux.HandleFunc("/readyz", h.handleReadyz)
	mux.Handle("/metrics", promhttp.HandlerFor(h.registry, promhttp.HandlerOpts{}))
	mux.HandleFunc("/v1/score", h.handleScore)
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

	score, features, err := h.scorer.Score(req.Reference, req.Distorted, req.Model)
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

// runHTTP starts the HTTP listener on addr and blocks until ctx is cancelled.
func runHTTP(
	ctx context.Context,
	addr string,
	srv *httpServer,
	log *slog.Logger,
) error {
	mux := http.NewServeMux()
	srv.routes(mux)

	httpSrv := &http.Server{
		Addr:              addr,
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
		WriteTimeout:      120 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	log.Info("HTTP server started", "addr", addr)

	// Graceful shutdown on context cancellation. The shutdown goroutine
	// intentionally derives its timeout context from context.Background()
	// rather than the request-scoped ctx: that ctx has *just* been cancelled
	// (we drain <-ctx.Done() below), so propagating it to Shutdown would
	// abort the in-flight-request drain immediately. This is the canonical
	// net/http graceful-shutdown pattern.
	// #nosec G118 -- intentional: see graceful-shutdown comment above.
	go func() { //nolint:contextcheck // see graceful-shutdown comment above
		<-ctx.Done()
		log.Info("HTTP graceful shutdown initiated")
		shutdownCtx, cancel := context.WithTimeout(context.Background(), observability.GracefulShutdownTimeout)
		defer cancel()
		if err := httpSrv.Shutdown(shutdownCtx); err != nil {
			log.Error("HTTP shutdown error", "error", err)
		}
	}()

	if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		return fmt.Errorf("http serve: %w", err)
	}
	return nil
}
