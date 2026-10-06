// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package observability

import (
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/prometheus/client_golang/prometheus"
)

// dashboardPath is the shipped Grafana dashboard, relative to this package.
const dashboardPath = "../../deploy/grafana/vmafx-overview.json"

// dashboardMetricRe picks the vmafx_* identifiers out of a PromQL expression.
var dashboardMetricRe = regexp.MustCompile(`vmafx_[a-zA-Z0-9_]+`)

type fakeQueue struct{}

func (fakeQueue) PendingCount() int { return 1 }
func (fakeQueue) RunningCount() int { return 2 }

type fakeNodes struct{}

func (fakeNodes) Count() int { return 3 }

// registeredSeries returns every Prometheus series name the vmafx binaries
// expose on /metrics: the families NewMetrics and SetControllerSources register,
// plus the _bucket / _sum / _count series of each histogram.
func registeredSeries(t *testing.T) map[string]bool {
	t.Helper()
	reg := prometheus.NewRegistry()
	m := NewMetrics(reg)
	m.ScoreDuration.Observe(0.1)
	m.SetControllerSources(fakeQueue{}, fakeNodes{})
	fams, err := reg.Gather()
	if err != nil {
		t.Fatalf("gather: %v", err)
	}
	out := map[string]bool{}
	for _, f := range fams {
		name := f.GetName()
		out[name] = true
		if f.GetType().String() == "HISTOGRAM" {
			for _, s := range []string{"_bucket", "_sum", "_count"} {
				out[name+s] = true
			}
		}
	}
	return out
}

// dashboardExprs returns every panel target expression of the dashboard.
func dashboardExprs(t *testing.T) []string {
	t.Helper()
	raw, err := os.ReadFile(filepath.FromSlash(dashboardPath))
	if err != nil {
		t.Fatalf("read dashboard: %v", err)
	}
	var d struct {
		Panels []struct {
			Title   string `json:"title"`
			Targets []struct {
				Expr string `json:"expr"`
			} `json:"targets"`
		} `json:"panels"`
	}
	if err := json.Unmarshal(raw, &d); err != nil {
		t.Fatalf("parse dashboard: %v", err)
	}
	var exprs []string
	for _, p := range d.Panels {
		for _, tg := range p.Targets {
			exprs = append(exprs, tg.Expr)
		}
	}
	return exprs
}

// TestDashboardQueriesOnlyRegisteredMetrics fails when a panel queries a
// vmafx_* series that no binary registers (issue #1251: a dashboard that
// shows "No data" on every panel is not a dashboard).
func TestDashboardQueriesOnlyRegisteredMetrics(t *testing.T) {
	t.Parallel()
	known := registeredSeries(t)
	exprs := dashboardExprs(t)
	if len(exprs) == 0 {
		t.Fatal("dashboard has no panel queries")
	}
	for _, e := range exprs {
		names := dashboardMetricRe.FindAllString(e, -1)
		if len(names) == 0 {
			t.Errorf("query %q names no vmafx_* metric", e)
		}
		for _, n := range names {
			if !known[n] {
				t.Errorf("query %q names unregistered metric %q", e, n)
			}
		}
	}
}

// TestDashboardCheckerRejectsUnknownMetric is the negative case: the matcher
// used above must flag a metric that is not registered.
func TestDashboardCheckerRejectsUnknownMetric(t *testing.T) {
	t.Parallel()
	known := registeredSeries(t)
	for _, n := range []string{"vmafx_jobs_in_flight", "vmafx_gpu_utilization"} {
		if known[n] {
			t.Errorf("%q unexpectedly registered", n)
		}
	}
	if !strings.Contains(strings.Join(mapKeys(known), " "), "vmafx_controller_jobs_pending") {
		t.Error("controller gauges missing from the registered set")
	}
}

func mapKeys(m map[string]bool) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	return out
}
