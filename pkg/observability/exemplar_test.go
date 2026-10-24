// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package observability

import (
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/prometheus/client_golang/prometheus"
	"go.opentelemetry.io/otel/trace"

	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// sampledContext returns a context carrying a sampled span context.
func sampledContext(t *testing.T, sampled bool) (context.Context, trace.TraceID) {
	t.Helper()
	tid, err := trace.TraceIDFromHex("4bf92f3577b34da6a3ce929d0e0e4736")
	if err != nil {
		t.Fatal(err)
	}
	sid, err := trace.SpanIDFromHex("00f067aa0ba902b7")
	if err != nil {
		t.Fatal(err)
	}
	var flags trace.TraceFlags
	if sampled {
		flags = trace.FlagsSampled
	}
	sc := trace.NewSpanContext(trace.SpanContextConfig{TraceID: tid, SpanID: sid, TraceFlags: flags})
	return trace.ContextWithSpanContext(context.Background(), sc), tid
}

// TestScoreDurationCarriesTheTraceAsExemplar: a sampled request's duration
// carries its trace id; an unsampled or traceless one carries none.
func TestScoreDurationCarriesTheTraceAsExemplar(t *testing.T) {
	t.Parallel()
	reg := prometheus.NewRegistry()
	m, err := NewMetrics(reg)
	if err != nil {
		t.Fatal(err)
	}
	ctx, tid := sampledContext(t, true)
	m.ObserveScoreDuration(ctx, 3)
	unsampled, _ := sampledContext(t, false)
	m.ObserveScoreDuration(unsampled, 3)
	m.ObserveScoreDuration(context.Background(), 3)

	fams, err := reg.Gather()
	if err != nil {
		t.Fatal(err)
	}
	var exemplars []string
	for _, f := range fams {
		if f.GetName() != metricdef.ServerScoreDuration.Name {
			continue
		}
		for _, b := range f.GetMetric()[0].GetHistogram().GetBucket() {
			for _, lp := range b.GetExemplar().GetLabel() {
				exemplars = append(exemplars, lp.GetName()+"="+lp.GetValue())
			}
		}
	}
	if len(exemplars) != 1 || exemplars[0] != ExemplarTraceID+"="+tid.String() {
		t.Errorf("exemplars = %v, want one trace_id=%s", exemplars, tid)
	}
}

// TestMetricsHandlerServesExemplarsAsOpenMetrics: a scraper that asks for
// OpenMetrics gets the exemplar on the bucket line.
func TestMetricsHandlerServesExemplarsAsOpenMetrics(t *testing.T) {
	t.Parallel()
	reg := prometheus.NewRegistry()
	m, err := NewMetrics(reg)
	if err != nil {
		t.Fatal(err)
	}
	ctx, tid := sampledContext(t, true)
	m.ObserveScoreDuration(ctx, 3)
	srv := httptest.NewServer(MetricsHandler(reg))
	defer srv.Close()
	req, err := http.NewRequestWithContext(context.Background(), http.MethodGet, srv.URL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Accept", "application/openmetrics-text;version=1.0.0")
	resp, err := srv.Client().Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if cerr := resp.Body.Close(); cerr != nil {
			t.Error(cerr)
		}
	}()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(body), `# {trace_id="`+tid.String()+`"}`) {
		t.Errorf("no exemplar in the OpenMetrics page:\n%s", body)
	}
}
