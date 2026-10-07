// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"bytes"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/observability"
)

// repoRoot is the repository root, relative to this package.
const repoRoot = "../../.."

// TestGeneratedFilesAreCurrent fails when a committed generated file differs
// from what Generate produces: edit metricdef or this package, then run
// go run ./tools/obsgen -write.
func TestGeneratedFilesAreCurrent(t *testing.T) {
	t.Parallel()
	files, err := Generate()
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range files {
		got, err := os.ReadFile(filepath.Join(repoRoot, filepath.FromSlash(f.Path)))
		if err != nil {
			t.Errorf("%s: %v; run go run ./tools/obsgen -write", f.Path, err)
			continue
		}
		if !bytes.Equal(got, f.Content) {
			t.Errorf("%s is stale; run go run ./tools/obsgen -write", f.Path)
		}
	}
}

// shippedDashboards returns every dashboard JSON under deploy/grafana.
func shippedDashboards(t *testing.T) []string {
	t.Helper()
	var paths []string
	err := filepath.WalkDir(filepath.Join(repoRoot, "deploy", "grafana"), func(p string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() && strings.HasSuffix(p, ".json") {
			paths = append(paths, p)
		}
		return err
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(paths) == 0 {
		t.Fatal("no dashboards under deploy/grafana")
	}
	return paths
}

// TestEveryDashboardQueryIsEmitted is the acceptance check of issue #2430:
// no shipped panel, annotation or variable queries a series nothing emits.
func TestEveryDashboardQueryIsEmitted(t *testing.T) {
	t.Parallel()
	for _, p := range shippedDashboards(t) {
		raw, err := os.ReadFile(p)
		if err != nil {
			t.Fatal(err)
		}
		problems, err := CheckDashboard(raw)
		if err != nil {
			t.Fatalf("%s: %v", p, err)
		}
		for _, msg := range problems {
			t.Errorf("%s: %s", p, msg)
		}
	}
}

// TestCheckRefusesTheDeadPanels is the negative case: the Overview dashboard
// as it was before generation queried five series nothing emits, and the
// check names each of them.
func TestCheckRefusesTheDeadPanels(t *testing.T) {
	t.Parallel()
	raw, err := os.ReadFile(filepath.Join("testdata", "overview-before-generation.json"))
	if err != nil {
		t.Fatal(err)
	}
	problems, err := CheckDashboard(raw)
	if err != nil {
		t.Fatal(err)
	}
	joined := strings.Join(problems, "\n")
	for _, dead := range []string{
		"vmafx_controller_jobs_queued", "vmafx_jobs_in_flight", "vmafx_controller_nodes_active",
		"vmafx_frames_per_second_bucket", "vmafx_gpu_utilization",
	} {
		if !strings.Contains(joined, " "+dead+",") {
			t.Errorf("dead series %s not reported; problems:\n%s", dead, joined)
		}
	}
	if len(problems) != 5 {
		t.Errorf("got %d problems, want 5:\n%s", len(problems), joined)
	}
}

func TestMetricNames(t *testing.T) {
	t.Parallel()
	cases := map[string][]string{
		`sum by (le, model) (rate(vmafx_quality_score_bucket{job=~"$job",model!~"a{b}"}[$__rate_interval]))`: {"vmafx_quality_score_bucket"},
		`histogram_quantile(0.99, sum by (le) (rate(x_bucket[5m]))) * 1000`:                                  {"x_bucket"},
		`count by (job) (up{job="a"} == 1) or vector(0)`:                                                     {"up"},
		`a / on (job) group_left (version) b offset 1h`:                                                      {"a", "b"},
		`max_over_time(c[1h:5m]) > bool 2 unless ignoring (x) d`:                                             {"c", "d"},
		`label_replace(e, "dst", "$1", "src", "(.*)") + ${var} + $__interval_ms + 1e3 + .5`:                  {"e"},
		`sum without (instance) (f) and topk(3, g) @ start()`:                                                {"f", "g"},
		`recorded:rule:rate5m{tenant=~"$tenant"} and recorded:rule:rate5m`:                                   {"recorded:rule:rate5m"},
	}
	for expr, want := range cases {
		if got := MetricNames(expr); !slices.Equal(got, want) {
			t.Errorf("MetricNames(%q) = %v, want %v", expr, got, want)
		}
	}
}

func TestCheckExprAcceptsDefinedAndExternalSeries(t *testing.T) {
	t.Parallel()
	if dead := CheckExpr(`sum(vmafx_controller_jobs_pending) + up + vmafx_controller_job_queue_wait_seconds_count`); len(dead) != 0 {
		t.Errorf("defined series reported dead: %v", dead)
	}
	if dead := CheckExpr(`vmafx_controller_job_queue_wait_seconds + nothing_emits_this`); !slices.Equal(dead,
		[]string{"vmafx_controller_job_queue_wait_seconds", "nothing_emits_this"}) {
		t.Errorf("CheckExpr = %v; a histogram's bare name and an unknown series are dead", dead)
	}
}

// TestExporterSeriesOnlyOnTheirDashboard: a vendor exporter's series is dead on
// a VMAFx dashboard and on another exporter's dashboard, and allowed on its own
// (tagged) one.
func TestExporterSeriesOnlyOnTheirDashboard(t *testing.T) {
	t.Parallel()
	expr := `avg(DCGM_FI_DEV_GPU_UTIL{job=~"$job"})`
	if dead := CheckExpr(expr); !slices.Equal(dead, []string{"DCGM_FI_DEV_GPU_UTIL"}) {
		t.Errorf("untagged: %v", dead)
	}
	if dead := CheckExpr(expr, "amd"); len(dead) != 1 {
		t.Errorf("another exporter's tag allowed it: %v", dead)
	}
	if dead := CheckExpr(expr, "dcgm"); len(dead) != 0 {
		t.Errorf("its own tag refused it: %v", dead)
	}
	raw := []byte(`{"title":"x","tags":["vmafx"],"panels":[{"title":"p","targets":[{"expr":"DCGM_FI_DEV_FB_USED"}]}]}`)
	if problems, err := CheckDashboard(raw); err != nil || len(problems) != 1 {
		t.Errorf("an exporter series on a VMAFx dashboard: %v, %v", problems, err)
	}
}

// TestExternalSeriesAreServed proves the allow-list: every collector series
// it names is on the registry every component serves.
func TestExternalSeriesAreServed(t *testing.T) {
	t.Parallel()
	reg, err := observability.NewRegistry()
	if err != nil {
		t.Fatal(err)
	}
	fams, err := reg.Gather()
	if err != nil {
		t.Fatal(err)
	}
	served := map[string]bool{}
	for _, f := range fams {
		served[f.GetName()] = true
	}
	for name := range ExternalSeries {
		if name == "up" {
			continue // Prometheus's own series, one per scrape target
		}
		if strings.HasPrefix(name, "process_") && runtime.GOOS != "linux" {
			continue // the process collector reads /proc; other systems serve fewer series
		}
		if !served[name] {
			t.Errorf("ExternalSeries names %s, which NewRegistry does not serve", name)
		}
	}
}
