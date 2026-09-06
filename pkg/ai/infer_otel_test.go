// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// infer_otel_test.go — Registry.Infer emits the ADR-0782 ONNX inference span
// around the vmafx-ort-runner subprocess (the runner itself is OTel-exempt,
// ADR-1134). Uses the same fake-runner harness as infer_runner_test.go.
//
// Not t.Parallel(): installs the process-global TracerProvider and PATH.

package ai

import (
	"context"
	"testing"

	"go.opentelemetry.io/otel/codes"

	"github.com/VMAFx/vmafx/internal/oteltest"
	"github.com/VMAFx/vmafx/pkg/observability"
)

func TestInfer_EmitsONNXInferenceSpan(t *testing.T) {
	sr := oteltest.Recorder(t)
	dir := t.TempDir()
	writeONNX(t, dir, "nr_metric_v1")
	r := NewRegistry(dir)

	installFakeRunner(t, "echo '[1.5]'")
	if _, err := r.Infer(context.Background(), "nr_metric_v1", []float64{1}); err != nil {
		t.Fatalf("Infer: %v", err)
	}
	spans := oteltest.Ended(sr, observability.SpanONNXInference)
	if len(spans) != 1 {
		t.Fatalf("want one %s span, got %v", observability.SpanONNXInference, oteltest.Names(sr))
	}
	if !oteltest.HasAttr(spans[0], observability.AttrModel, "nr_metric_v1") {
		t.Errorf("span attributes %v lack %s=nr_metric_v1", spans[0].Attributes(), observability.AttrModel)
	}
	if spans[0].Status().Code != codes.Ok {
		t.Errorf("successful inference span status %v, want Ok", spans[0].Status())
	}

	installFakeRunner(t, "echo 'libvmaf was built without DNN support' >&2; exit 3")
	if _, err := r.Infer(context.Background(), "nr_metric_v1", []float64{1}); err == nil {
		t.Fatal("expected an error from a runner exiting 3")
	}
	spans = oteltest.Ended(sr, observability.SpanONNXInference)
	if len(spans) != 2 {
		t.Fatalf("want a second span for the failed run, got %v", oteltest.Names(sr))
	}
	if spans[1].Status().Code != codes.Error {
		t.Errorf("failed inference span status %v, want Error", spans[1].Status())
	}
}
