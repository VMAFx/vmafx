// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

//go:build cgo

package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"testing"
	"time"

	"go.uber.org/fx/fxtest"

	"github.com/golusoris/golusoris/core/config"
	"github.com/golusoris/golusoris/httpx/server"

	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// TestNodeServesEveryFamilyItEmits is the contract between metricdef and the
// node: after one job of each outcome, every family metricdef lists for the
// node is on its registry, and no vmafx_ family is there that metricdef does
// not list for it.
func TestNodeServesEveryFamilyItEmits(t *testing.T) {
	reg, err := provideNodeRegistry()
	if err != nil {
		t.Fatal(err)
	}
	m, err := newNodeMetrics(reg, "cuda", func(context.Context) ([]deviceMemory, error) {
		return []deviceMemory{{device: "0 test GPU", used: 1 << 30, total: 8 << 30}}, nil
	})
	if err != nil {
		t.Fatal(err)
	}
	streams, err := provideStreamMetrics(reg)
	if err != nil {
		t.Fatal(err)
	}
	m.setSlots(2)
	for _, jobErr := range []error{nil, errors.New("vmaf exited 1"), fmt.Errorf("x: %w", errCancelledByController)} {
		m.jobStarted()
		m.jobDone("cuda", jobErr, time.Second)
	}
	session := streams.Begin()
	session.Frame()
	session.End(nil)
	fams, err := reg.Gather()
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]bool{}
	for _, f := range fams {
		got[f.GetName()] = true
	}
	defined := map[string]bool{}
	for _, f := range metricdef.ByEmitter(metricdef.Node) {
		defined[f.Name] = true
		if !got[f.Name] {
			t.Errorf("metricdef lists %s for the node, its registry does not serve it", f.Name)
		}
	}
	for name := range got {
		if metricdef.IsVMAFx(name) && !defined[name] {
			t.Errorf("the node serves %s, which metricdef does not list for it", name)
		}
	}
}

// TestControllerClientRecordsJobMetrics: a job the client pulls and runs is
// counted under its backend and outcome, timed, and leaves no running job.
func TestControllerClientRecordsJobMetrics(t *testing.T) {
	t.Parallel()
	reg, err := provideNodeRegistry()
	if err != nil {
		t.Fatal(err)
	}
	m, err := newNodeMetrics(reg, "cpu", nil)
	if err != nil {
		t.Fatal(err)
	}
	f := newFakeController()
	f.onPull = oneJob(scoringJob("job-m"), "")
	startTestClientWith(t, testControllerConfig(serveFake(t, f)), constantResult(42.5), m)
	awaitReport(t, f, 5*time.Second)

	fams, err := reg.Gather()
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, fam := range fams {
		got[fam.GetName()] = fam.String()
	}
	if !strings.Contains(got[metricdef.NodeJobs.Name], `value:"completed"`) ||
		!strings.Contains(got[metricdef.NodeJobDuration.Name], "sample_count:1") ||
		!strings.Contains(got[metricdef.NodeJobsRunning.Name], "value:0") {
		t.Errorf("node job families after one job:\n%s\n%s\n%s", got[metricdef.NodeJobs.Name],
			got[metricdef.NodeJobDuration.Name], got[metricdef.NodeJobsRunning.Name])
	}
}

func TestJobOutcome(t *testing.T) {
	t.Parallel()
	cases := map[string]error{
		"completed": nil,
		"failed":    errors.New("boom"),
		"cancelled": fmt.Errorf("%w: context canceled", errCancelledByController),
	}
	for want, err := range cases {
		if got := jobOutcome(err); got != want {
			t.Errorf("jobOutcome(%v) = %q, want %q", err, got, want)
		}
	}
}

// TestNilNodeMetricsAreNoOps: a client built without metrics (unit tests of
// the controller client) records nothing and does not panic.
func TestNilNodeMetricsAreNoOps(t *testing.T) {
	t.Parallel()
	var m *nodeMetrics
	m.setSlots(1)
	m.jobStarted()
	m.jobDone("cpu", nil, time.Second)
}

// TestNodeHTTPServesMetricsAndProbes boots the production graph and reads the
// node's HTTP surface: /metrics with the node families, /readyz ready (the
// scorer is configured).
func TestNodeHTTPServesMetricsAndProbes(t *testing.T) {
	writeNodeEnv(t)
	app := fxtest.New(t, productionGraph())
	app.RequireStart()
	defer app.RequireStop()
	base := "http://" + os.Getenv("VMAFX_HTTP_ADDR")

	body := httpGet(t, base+"/metrics", http.StatusOK)
	for _, want := range []string{
		`vmafx_node_info{backend="cpu",vendor="cpu"} 1`,
		"vmafx_node_slots 0",
		"vmafx_node_jobs_running 0",
		"go_goroutines",
	} {
		if !strings.Contains(body, want) {
			t.Errorf("/metrics lacks %q", want)
		}
	}
	httpGet(t, base+"/readyz", http.StatusOK)
	httpGet(t, base+"/livez", http.StatusOK)
}

func httpGet(t *testing.T, url string, wantStatus int) string {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		t.Fatal(err)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("GET %s: %v", url, err)
	}
	defer func() {
		if cerr := resp.Body.Close(); cerr != nil {
			t.Errorf("close body: %v", cerr)
		}
	}()
	b, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read %s: %v", url, err)
	}
	if resp.StatusCode != wantStatus {
		t.Fatalf("GET %s = %d, want %d: %s", url, resp.StatusCode, wantStatus, b)
	}
	return string(b)
}

// TestWithNodeHTTPDefault: without VMAFX_HTTP_ADDR the node listens on the
// chart's metrics port; an explicit address is kept.
func TestWithNodeHTTPDefault(t *testing.T) {
	t.Run("missing uses node default", func(t *testing.T) {
		t.Setenv("VMAFX_HTTP_ADDR", "")
		raw, err := config.New(nodeEnvOptions(false))
		if err != nil {
			t.Fatalf("config.New: %v", err)
		}
		if got := withNodeHTTPDefault(server.DefaultOptions(), raw); got.Addr != defaultNodeHTTPAddr {
			t.Errorf("Addr = %q, want %q", got.Addr, defaultNodeHTTPAddr)
		}
	})
	t.Run("explicit override is preserved", func(t *testing.T) {
		const override = "127.0.0.1:9464"
		t.Setenv("VMAFX_HTTP_ADDR", override)
		raw, err := config.New(nodeEnvOptions(false))
		if err != nil {
			t.Fatalf("config.New: %v", err)
		}
		if got := withNodeHTTPDefault(server.Options{Addr: override}, raw); got.Addr != override {
			t.Errorf("Addr = %q, want explicit override %q", got.Addr, override)
		}
	})
}
