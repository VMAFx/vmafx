// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/http_server.go — HTTP handlers for vmafx-controller.
//
// Endpoints:
//   GET  /healthz      — liveness probe; always 200 while the process is live.
//   GET  /readyz       — readiness probe; 200 once vmaf binary is reachable.
//   GET  /metrics      — Prometheus exposition format.
//   POST /v1/score     — JSON scoring endpoint (direct scoring, Phase 4a compat).
//
// ADR-0703: vmafx-server Go gRPC + HTTP service (origin).
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.

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

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// maxScoreRequestBodyBytes caps the request body for POST /v1/score.
// Mirrors the same limit in cmd/vmafx-server/http_server.go (ADR-1065).
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
	authMW   *auth.Middleware
}

// newHTTPServer creates an httpServer.
func newHTTPServer(
	scorer *libvmaf.Scorer,
	metrics *observability.Metrics,
	registry *prometheus.Registry,
	authMW *auth.Middleware,
	log *slog.Logger,
) *httpServer {
	return &httpServer{
		scorer:   scorer,
		metrics:  metrics,
		log:      log,
		registry: registry,
		authMW:   authMW,
	}
}

// routes registers all HTTP handlers on mux.
// The auth middleware wraps all handlers; /healthz, /readyz, and /metrics
// are exempted inside the middleware (ADR-0794).
func (h *httpServer) routes(mux *http.ServeMux) {
	mux.HandleFunc("/healthz", h.handleHealthz)
	mux.HandleFunc("/readyz", h.handleReadyz)
	mux.Handle("/metrics", promhttp.HandlerFor(h.registry, promhttp.HandlerOpts{}))

	// /v1/score requires at least vmafx:writer (or vmafx:admin).
	scoreHandler := http.HandlerFunc(h.handleScore)
	if h.authMW != nil {
		requireWriter := h.authMW.RequireRole(auth.RoleWriter, auth.RoleAdmin)
		mux.Handle("/v1/score", h.authMW.HTTPHandler(requireWriter(scoreHandler)))
	} else {
		mux.HandleFunc("/v1/score", h.handleScore)
	}
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
	// to the decoder as *http.MaxBytesError, which we map to 413 below.
	// ADR-1065: mirrors the same guard in cmd/vmafx-server/http_server.go.
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

// Serving note (ADR-1119): the controller no longer hand-rolls an *http.Server.
// golusoris httpx/server.Module owns the listener (OnStart bind, OnStop graceful
// drain) and the chi router. The handlers above are mounted onto that router by
// mountControllerHTTP in main.go. The routes() method below is retained as a
// net/http test harness so the handler-level tests can exercise the endpoints
// against an in-process ServeMux without standing up the full fx graph.
