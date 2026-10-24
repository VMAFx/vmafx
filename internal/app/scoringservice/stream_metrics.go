// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package scoringservice

import (
	"context"
	"errors"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/VMAFx/vmafx/pkg/observability"
	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// StreamMetrics are the live ScoreStream families (metricdef.StreamSessions
// and its siblings) that vmafx-server and vmafx-node record, so the two
// ScoreStream handlers count sessions one way.
type StreamMetrics struct {
	open     observability.Gauge
	finished observability.Counter
	frames   observability.Counter
	duration observability.Histogram
}

// NewStreamMetrics registers the ScoreStream families on reg.
func NewStreamMetrics(reg prometheus.Registerer) (*StreamMetrics, error) {
	open, err := observability.NewGauge(reg, metricdef.StreamSessions)
	if err != nil {
		return nil, err
	}
	finished, err := observability.NewCounter(reg, metricdef.StreamSessionsFinished)
	if err != nil {
		return nil, err
	}
	frames, err := observability.NewCounter(reg, metricdef.StreamFrames)
	if err != nil {
		return nil, err
	}
	duration, err := observability.NewHistogram(reg, metricdef.StreamSessionDuration)
	if err != nil {
		return nil, err
	}
	open.Set(0)
	return &StreamMetrics{open: open, finished: finished, frames: frames, duration: duration}, nil
}

// StreamSession records one ScoreStream session from Begin to End.
type StreamSession struct {
	m     *StreamMetrics
	ctx   context.Context
	start time.Time
}

// Begin counts a session that opened; ctx is the session's call, whose trace
// becomes the duration's exemplar. A nil receiver (a handler built without
// metrics in a test) returns a session that records nothing.
func (m *StreamMetrics) Begin(ctx context.Context) *StreamSession {
	if m == nil {
		return &StreamSession{}
	}
	m.open.Add(1)
	return &StreamSession{m: m, ctx: ctx, start: time.Now()}
}

// Frame counts one frame pair the session received.
func (s *StreamSession) Frame() {
	if s.m != nil {
		s.m.frames.Inc()
	}
}

// End records how the session ended: err nil is completed, a cancelled or
// expired context (or the gRPC status for one) is cancelled, anything else
// failed.
func (s *StreamSession) End(err error) {
	if s.m == nil {
		return
	}
	s.m.open.Add(-1)
	s.m.finished.Inc(streamOutcome(err))
	s.m.duration.ObserveContext(s.ctx, time.Since(s.start).Seconds())
}

func streamOutcome(err error) string {
	switch {
	case err == nil:
		return "completed"
	case errors.Is(err, context.Canceled), errors.Is(err, context.DeadlineExceeded):
		return "cancelled"
	}
	switch status.Code(err) {
	case codes.Canceled, codes.DeadlineExceeded:
		return "cancelled"
	default:
		return "failed"
	}
}
