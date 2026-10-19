// SPDX-License-Identifier: EUPL-1.2
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
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/internal/app/scoringservice"
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

// scoreResponse is the /v1/score JSON response body as legacy clients read it
// (score, features); the served body is the proto ScoreResponse in JSON, which
// carries these two fields and the provenance (#2155).
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
	scoringservice.HandleHealthz(h.metrics, h.log, w, r)
}

// handleReadyz is the readiness probe — returns 503 until the scorer is usable.
func (h *httpServer) handleReadyz(w http.ResponseWriter, r *http.Request) {
	scoringservice.HandleReadyz(h.metrics, h.log, h.scorer != nil, w, r)
}

// scoreLogger is the request-scoped logger for POST /v1/score: it carries the
// shared request_id and route fields (pkg/observability/logfields.go).
func (h *httpServer) scoreLogger(r *http.Request) *slog.Logger {
	return observability.RouteLogger(r.Context(), h.log, http.MethodPost, "/v1/score")
}

// handleScore handles POST /v1/score.
func (h *httpServer) handleScore(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	h.metrics.ScoreRequests.Inc()
	start := time.Now()
	req, ok := h.decodeScoreRequest(w, r)
	if !ok {
		return
	}
	release, ok := h.acquireScoreSlot(w, r)
	if !ok {
		return
	}
	defer release()
	h.scoreAndRespond(w, r, req, start)
}

// decodeScoreRequest reads the body as the contract's ScoreRequest
// (reference, distorted, model, options; proto field names). A field the
// contract does not know is refused.
func (h *httpServer) decodeScoreRequest(w http.ResponseWriter, r *http.Request) (*vmafxv1.ScoreRequest, bool) {
	// Cap the request body at maxScoreRequestBodyBytes. http.MaxBytesReader
	// closes the underlying body when the limit trips and surfaces the cause
	// to the reader as `*http.MaxBytesError`, which we map to 413 below.
	r.Body = http.MaxBytesReader(w, r.Body, maxScoreRequestBodyBytes)
	body, err := io.ReadAll(r.Body)
	if _, tooLarge := errors.AsType[*http.MaxBytesError](err); tooLarge {
		h.metrics.ScoreErrors.Inc()
		scoringservice.WriteJSON(h.log, w, http.StatusRequestEntityTooLarge, errorResponse{
			Error: fmt.Sprintf("request body exceeds %d bytes", maxScoreRequestBodyBytes),
		})
		return nil, false
	}
	req := &vmafxv1.ScoreRequest{}
	if err == nil {
		err = requestJSONOptions.Unmarshal(body, req)
	}
	if err != nil {
		h.metrics.ScoreErrors.Inc()
		scoringservice.WriteJSON(h.log, w, http.StatusBadRequest,
			errorResponse{Error: fmt.Sprintf("invalid JSON body: %v", err)})
		return nil, false
	}
	if req.GetReference() == "" || req.GetDistorted() == "" {
		h.metrics.ScoreErrors.Inc()
		scoringservice.WriteJSON(h.log, w, http.StatusBadRequest,
			errorResponse{Error: "reference and distorted are required"})
		return nil, false
	}
	return req, true
}

func (h *httpServer) acquireScoreSlot(w http.ResponseWriter, r *http.Request) (func(), bool) {
	if h.limiter == nil {
		return func() {}, true
	}
	if err := h.limiter.Acquire(r.Context()); err != nil {
		h.metrics.ScoreErrors.Inc()
		h.scoreLogger(r).Warn("http Score rejected: concurrency cap reached",
			"max", h.limiter.Max(), observability.FieldError, err)
		scoringservice.WriteJSON(h.log, w, http.StatusTooManyRequests, errorResponse{
			Error: fmt.Sprintf("too many concurrent scoring requests (max %d); try again later", h.limiter.Max()),
		})
		return nil, false
	}
	return h.limiter.Release, true
}

func (h *httpServer) scoreAndRespond(
	w http.ResponseWriter, r *http.Request, req *vmafxv1.ScoreRequest, start time.Time,
) {
	// The request-scoped context reaches the vmaf subprocess through
	// runScore, the scoring path gRPC Score uses too.
	resp, err := runScore(r.Context(), h.scorer, req)
	took := time.Since(start)
	h.metrics.ScoreDuration.Observe(took.Seconds())

	if err != nil {
		h.metrics.ScoreErrors.Inc()
		h.scoreLogger(r).Error("http Score failed",
			observability.FieldModel, req.GetModel(), observability.FieldError, err,
			observability.Seconds(took))
		scoringservice.WriteJSON(h.log, w, httpStatusOf(err),
			errorResponse{Error: status.Convert(err).Message()})
		return
	}

	h.metrics.ObserveScore("", req.GetModel(), resp.GetScore())
	h.scoreLogger(r).Info("http Score completed",
		"score", fmt.Sprintf("%.4f", resp.GetScore()),
		observability.FieldModel, resp.GetProvenance().GetModel(),
		observability.FieldBackend, resp.GetProvenance().GetBackendUsed(),
		observability.Seconds(took),
	)
	writeProtoJSON(h.log, w, resp)
}

// writeProtoJSON writes a response message as JSON with proto field names.
func writeProtoJSON(log *slog.Logger, w http.ResponseWriter, msg proto.Message) {
	body, err := responseJSONOptions.Marshal(msg)
	if err != nil {
		scoringservice.WriteJSON(log, w, http.StatusInternalServerError,
			errorResponse{Error: fmt.Sprintf("encode response: %v", err)})
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	if _, err := w.Write(body); err != nil {
		log.Warn("write /v1/score response", observability.FieldError, err)
	}
}
