// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package scoringservice

import (
	"encoding/json"
	"log/slog"
	"net/http"

	"github.com/VMAFx/vmafx/pkg/observability"
)

var (
	healthyResponse  = []byte(`{"status":"ok"}`)
	readyResponse    = []byte(`{"status":"ready"}`)
	notReadyResponse = []byte(`{"status":"not ready","reason":"scorer not initialised"}`)
)

// HandleHealthz serves the common legacy liveness contract.
func HandleHealthz(
	metrics *observability.Metrics,
	log *slog.Logger,
	w http.ResponseWriter,
	r *http.Request,
) {
	metrics.HealthRequests.Inc()
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	writeStaticJSON(log, w, http.StatusOK, healthyResponse)
}

// HandleReadyz serves the common legacy readiness contract.
func HandleReadyz(
	metrics *observability.Metrics,
	log *slog.Logger,
	ready bool,
	w http.ResponseWriter,
	r *http.Request,
) {
	metrics.ReadyRequests.Inc()
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if !ready {
		writeStaticJSON(log, w, http.StatusServiceUnavailable, notReadyResponse)
		return
	}
	writeStaticJSON(log, w, http.StatusOK, readyResponse)
}

// WriteJSON serialises v using the response formatting shared by every VMAFx
// Go scoring endpoint. Encoding and socket write failures are logged instead
// of being silently discarded after the response status has been committed.
func WriteJSON(log *slog.Logger, w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		logger(log).Error("write JSON response", "error", err)
	}
}

func writeStaticJSON(log *slog.Logger, w http.ResponseWriter, code int, body []byte) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	if _, err := w.Write(body); err != nil {
		logger(log).Error("write probe response", "error", err)
	}
}

func logger(log *slog.Logger) *slog.Logger {
	if log == nil {
		return slog.Default()
	}
	return log
}
