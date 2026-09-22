// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package scoringservice

import (
	"bytes"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/VMAFx/vmafx/pkg/observability"
)

func TestLegacyProbeContracts(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name       string
		serve      func(*observability.Metrics, *slog.Logger, http.ResponseWriter, *http.Request)
		method     string
		wantStatus int
		wantBody   string
	}{
		{
			name:       "health",
			serve:      HandleHealthz,
			method:     http.MethodGet,
			wantStatus: http.StatusOK,
			wantBody:   `{"status":"ok"}`,
		},
		{
			name: "ready",
			serve: func(m *observability.Metrics, l *slog.Logger, w http.ResponseWriter, r *http.Request) {
				HandleReadyz(m, l, true, w, r)
			},
			method:     http.MethodGet,
			wantStatus: http.StatusOK,
			wantBody:   `{"status":"ready"}`,
		},
		{
			name: "not ready",
			serve: func(m *observability.Metrics, l *slog.Logger, w http.ResponseWriter, r *http.Request) {
				HandleReadyz(m, l, false, w, r)
			},
			method:     http.MethodGet,
			wantStatus: http.StatusServiceUnavailable,
			wantBody:   `{"status":"not ready","reason":"scorer not initialised"}`,
		},
		{
			name:       "method rejected",
			serve:      HandleHealthz,
			method:     http.MethodPost,
			wantStatus: http.StatusMethodNotAllowed,
			wantBody:   "method not allowed\n",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			metrics := observability.NewMetrics(prometheus.NewRegistry())
			recorder := httptest.NewRecorder()
			tc.serve(metrics, slog.New(slog.NewTextHandler(io.Discard, nil)), recorder,
				httptest.NewRequest(tc.method, "/", nil))
			if recorder.Code != tc.wantStatus {
				t.Fatalf("status = %d, want %d", recorder.Code, tc.wantStatus)
			}
			if recorder.Body.String() != tc.wantBody {
				t.Fatalf("body = %q, want %q", recorder.Body.String(), tc.wantBody)
			}
		})
	}
}

func TestResponseWriteFailuresAreLogged(t *testing.T) {
	t.Parallel()

	var logs bytes.Buffer
	log := slog.New(slog.NewTextHandler(&logs, nil))
	w := failingResponseWriter{header: make(http.Header)}
	metrics := observability.NewMetrics(prometheus.NewRegistry())

	HandleHealthz(metrics, log, w, httptest.NewRequest(http.MethodGet, "/healthz", nil))
	WriteJSON(log, w, http.StatusOK, map[string]string{"status": "ok"})

	got := logs.String()
	for _, message := range []string{"write probe response", "write JSON response"} {
		if !strings.Contains(got, message) {
			t.Errorf("logs %q do not contain %q", got, message)
		}
	}
}

type failingResponseWriter struct {
	header http.Header
}

func (w failingResponseWriter) Header() http.Header { return w.header }

func (failingResponseWriter) WriteHeader(int) {}

func (failingResponseWriter) Write([]byte) (int, error) {
	return 0, errors.New("closed connection")
}
