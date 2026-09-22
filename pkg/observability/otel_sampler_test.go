// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// pkg/observability/otel_sampler_test.go — regression coverage for how
// OTEL_TRACES_SAMPLER_ARG is resolved into a trace sample ratio.
//
// The contract (ADR-0927) is an *accept* predicate: the environment value
// wins only when it parses AND lands inside [0, 1]; anything else leaves
// DefaultTraceSampleRatio standing. Rewriting that as a reject predicate is
// not a De Morgan dual, because NaN compares false against every bound:
// strconv.ParseFloat("NaN", 64) returns NaN with a nil error, so a reject
// predicate finds nothing to reject and lets NaN through to the sampler,
// where uint64(NaN * (1<<63)) is implementation-defined.
//
// The test drives the exported InitOTel and reads the ratio back off the
// "otel: initialised" log record, so it is a black-box assertion on the
// package's observable behaviour and does not depend on how the resolution
// is factored internally.

package observability

import (
	"context"
	"log/slog"
	"math"
	"testing"
)

// ratioCaptureHandler is a minimal slog.Handler that remembers the
// trace_sample_ratio attribute InitOTel logs when it initialises.
type ratioCaptureHandler struct {
	seen  bool
	ratio float64
}

func (h *ratioCaptureHandler) Enabled(context.Context, slog.Level) bool { return true }

func (h *ratioCaptureHandler) Handle(_ context.Context, rec slog.Record) error {
	rec.Attrs(func(a slog.Attr) bool {
		if a.Key != "trace_sample_ratio" {
			return true
		}
		h.seen = true
		h.ratio = a.Value.Float64()
		return false
	})
	return nil
}

func (h *ratioCaptureHandler) WithAttrs([]slog.Attr) slog.Handler { return h }

func (h *ratioCaptureHandler) WithGroup(string) slog.Handler { return h }

// TestInitOTel_SamplerArgResolution pins the full accept/reject table for
// OTEL_TRACES_SAMPLER_ARG, NaN and the infinities included.
func TestInitOTel_SamplerArgResolution(t *testing.T) {
	cases := []struct {
		name string
		raw  string
		want float64
	}{
		{"unset", "", DefaultTraceSampleRatio},
		{"in range", "0.5", 0.5},
		{"lower bound", "0", 0},
		{"upper bound", "1", 1},
		{"above range", "9.5", DefaultTraceSampleRatio},
		{"below range", "-0.1", DefaultTraceSampleRatio},
		{"unparsable", "garbage", DefaultTraceSampleRatio},
		{"nan uppercase", "NaN", DefaultTraceSampleRatio},
		{"nan lowercase", "nan", DefaultTraceSampleRatio},
		{"positive infinity", "+Inf", DefaultTraceSampleRatio},
		{"negative infinity", "-Inf", DefaultTraceSampleRatio},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			resetGlobals()
			withEnv(t,
				"OTEL_EXPORTER_OTLP_ENDPOINT", "http://127.0.0.1:14318",
				"OTEL_SDK_DISABLED", "",
				"OTEL_TRACES_SAMPLER_ARG", tc.raw,
			)

			capture := &ratioCaptureHandler{}
			sd := InitOTel(context.Background(), "test-service", slog.New(capture))
			if sd == nil {
				t.Fatalf("InitOTel returned nil shutdown")
			}
			defer shutdownBounded(t, sd)

			if !capture.seen {
				t.Fatalf("InitOTel logged no trace_sample_ratio attribute")
			}
			if math.IsNaN(capture.ratio) {
				t.Fatalf("OTEL_TRACES_SAMPLER_ARG=%q resolved to NaN; the sampler "+
					"argument must fall back to DefaultTraceSampleRatio instead", tc.raw)
			}
			if capture.ratio != tc.want {
				t.Errorf("OTEL_TRACES_SAMPLER_ARG=%q resolved to %v, want %v",
					tc.raw, capture.ratio, tc.want)
			}
		})
	}
}
