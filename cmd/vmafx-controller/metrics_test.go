// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

//go:build cgo

package main

import (
	"testing"

	"github.com/prometheus/client_golang/prometheus"
	dto "github.com/prometheus/client_model/go"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/observability"
	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// lifecycleNode registers one node of the test tenant with two slots.
func lifecycleNode(t *testing.T, f *grpcFixture) *controllerv1.RegisterNodeResponse {
	t.Helper()
	node, err := f.srv.RegisterNode(testTenantCtx(), &controllerv1.RegisterNodeRequest{
		Name:       "metrics-node",
		Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 2},
	})
	if err != nil {
		t.Fatalf("RegisterNode: %v", err)
	}
	return node
}

// runLifecycle submits four jobs and drives them to completed, failed,
// cancelled and pending, repeating the final report and the cancel once each.
func runLifecycle(t *testing.T, f *grpcFixture) {
	t.Helper()
	ctx := testTenantCtx()
	var ids []string
	for range 4 {
		sub, err := f.srv.SubmitJob(ctx, &controllerv1.SubmitJobRequest{
			Scoring: &controllerv1.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv", Model: "vmaf_v0.6.1"},
		})
		if err != nil {
			t.Fatalf("SubmitJob: %v", err)
		}
		ids = append(ids, sub.GetJobId())
	}
	node := lifecycleNode(t, f)
	for range 2 {
		if _, err := f.srv.PullWork(ctx, &controllerv1.PullWorkRequest{
			NodeId: node.GetNodeId(), SessionToken: node.GetSessionToken(),
			Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 2},
		}); err != nil {
			t.Fatalf("PullWork: %v", err)
		}
	}
	report := func(id string, score float64, errMsg string) {
		if _, err := f.srv.ReportResult(ctx, &controllerv1.ReportResultRequest{
			NodeId: node.GetNodeId(), SessionToken: node.GetSessionToken(),
			JobId: id, Final: true, Score: score, Error: errMsg,
		}); err != nil {
			t.Fatalf("ReportResult(%s): %v", id, err)
		}
	}
	report(ids[0], 91, "")
	report(ids[0], 91, "") // retry: idempotent, counted once
	report(ids[1], 0, "vmaf exited 1")
	for range 2 { // the second cancel is a no-op on a cancelled job
		if _, err := f.srv.CancelJob(ctx, &controllerv1.CancelJobRequest{JobId: ids[2]}); err != nil {
			t.Fatalf("CancelJob: %v", err)
		}
	}
}

// gathered maps each family name to its metrics.
func gathered(t *testing.T, reg *prometheus.Registry) map[string][]*dto.Metric {
	t.Helper()
	fams, err := reg.Gather()
	if err != nil {
		t.Fatalf("Gather: %v", err)
	}
	out := map[string][]*dto.Metric{}
	for _, f := range fams {
		out[f.GetName()] = f.GetMetric()
	}
	return out
}

// sampleValue is the counter or gauge value, or the histogram sample count,
// summed over every series of a family.
func sampleValue(ms []*dto.Metric) float64 {
	v := 0.0
	for _, m := range ms {
		switch {
		case m.GetCounter() != nil:
			v += m.GetCounter().GetValue()
		case m.GetGauge() != nil:
			v += m.GetGauge().GetValue()
		case m.GetHistogram() != nil:
			v += float64(m.GetHistogram().GetSampleCount())
		}
	}
	return v
}

// TestJobLifecycleMetrics drives jobs through every terminal state over the
// gRPC handlers and checks each family counts what happened, once.
func TestJobLifecycleMetrics(t *testing.T) {
	reg, err := observability.NewRegistry()
	if err != nil {
		t.Fatal(err)
	}
	f := newGRPCFixtureOn(t, reg)
	if err := registerQueueCollector(reg, f.queue, f.registry); err != nil {
		t.Fatal(err)
	}
	runLifecycle(t, f)
	got := gathered(t, reg)
	want := map[string]float64{
		metricdef.ControllerJobsSubmitted.Name:  4,
		metricdef.ControllerJobsCompleted.Name:  1,
		metricdef.ControllerJobsFailed.Name:     1,
		metricdef.ControllerJobsCancelled.Name:  1,
		metricdef.ControllerJobQueueWait.Name:   2, // two jobs assigned
		metricdef.ControllerJobDuration.Name:    3, // completed, failed, cancelled
		metricdef.QualityScore.Name:             1, // the completed job's score
		metricdef.ControllerJobsPending.Name:    1,
		metricdef.ControllerJobsRunning.Name:    0,
		metricdef.ControllerJobsRequeued.Name:   0,
		metricdef.ControllerNodesLive.Name:      1,
		metricdef.ControllerQueueOldestAge.Name: -1, // present; value is an age
	}
	for name, w := range want {
		ms, ok := got[name]
		if !ok {
			t.Errorf("%s missing from /metrics", name)
			continue
		}
		if v := sampleValue(ms); w >= 0 && v != w {
			t.Errorf("%s = %v, want %v", name, v, w)
		}
	}
}

// TestControllerServesEveryFamilyItEmits is the contract between metricdef
// and the controller: after one lifecycle, every family metricdef lists for
// the controller is on its /metrics page, and no vmafx_ family is there that
// metricdef does not list for it.
func TestControllerServesEveryFamilyItEmits(t *testing.T) {
	reg, err := observability.NewRegistry()
	if err != nil {
		t.Fatal(err)
	}
	f := newGRPCFixtureOn(t, reg)
	if err := registerQueueCollector(reg, f.queue, f.registry); err != nil {
		t.Fatal(err)
	}
	runLifecycle(t, f)
	got := gathered(t, reg)
	defined := map[string]bool{}
	for _, fam := range metricdef.ByEmitter(metricdef.Controller) {
		defined[fam.Name] = true
		if _, ok := got[fam.Name]; !ok {
			t.Errorf("metricdef lists %s for the controller, /metrics does not serve it", fam.Name)
		}
	}
	for name := range got {
		if metricdef.IsVMAFx(name) && !defined[name] {
			t.Errorf("/metrics serves %s, which metricdef does not list for the controller", name)
		}
	}
}
