// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package scoringservice

import (
	"context"
	"errors"
	"fmt"
	"testing"

	"github.com/prometheus/client_golang/prometheus"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

func TestStreamOutcome(t *testing.T) {
	t.Parallel()
	cases := map[error]string{
		nil:              "completed",
		context.Canceled: "cancelled",
		fmt.Errorf("x: %w", context.DeadlineExceeded):    "cancelled",
		status.Error(codes.Canceled, "gone"):             "cancelled",
		status.Error(codes.InvalidArgument, "bad frame"): "failed",
		errors.New("scorer exploded"):                    "failed",
	}
	for err, want := range cases {
		if got := streamOutcome(err); got != want {
			t.Errorf("streamOutcome(%v) = %q, want %q", err, got, want)
		}
	}
}

// TestStreamSessionCounts: a session counts open while it runs, its frames,
// and its outcome and duration when it ends; a nil StreamMetrics records
// nothing and does not panic.
func TestStreamSessionCounts(t *testing.T) {
	t.Parallel()
	reg := prometheus.NewRegistry()
	m, err := NewStreamMetrics(reg)
	if err != nil {
		t.Fatal(err)
	}
	s := m.Begin(context.Background())
	s.Frame()
	s.Frame()
	if open := gatherValue(t, reg, metricdef.StreamSessions.Name); open != 1 {
		t.Errorf("open while running = %v, want 1", open)
	}
	s.End(status.Error(codes.Internal, "boom"))
	if open := gatherValue(t, reg, metricdef.StreamSessions.Name); open != 0 {
		t.Errorf("open after End = %v, want 0", open)
	}
	if frames := gatherValue(t, reg, metricdef.StreamFrames.Name); frames != 2 {
		t.Errorf("frames = %v, want 2", frames)
	}
	if failed := gatherValue(t, reg, metricdef.StreamSessionsFinished.Name); failed != 1 {
		t.Errorf("finished = %v, want 1", failed)
	}
	var none *StreamMetrics
	n := none.Begin(context.Background())
	n.Frame()
	n.End(nil)
}

// gatherValue sums the counter or gauge values of family name.
func gatherValue(t *testing.T, reg *prometheus.Registry, name string) float64 {
	t.Helper()
	fams, err := reg.Gather()
	if err != nil {
		t.Fatal(err)
	}
	v := 0.0
	for _, f := range fams {
		if f.GetName() != name {
			continue
		}
		for _, m := range f.GetMetric() {
			v += m.GetCounter().GetValue() + m.GetGauge().GetValue()
		}
	}
	return v
}
