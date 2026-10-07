// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"bytes"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"slices"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/observability"
	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
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

// TestCheckRefusesWholeNumberBucketMatchers: a dashboard matching le="70"
// shows no data on Prometheus 3 (which stores le="70.0"); the check reports
// it and accepts LeMatcher and a fractional bound.
func TestCheckRefusesWholeNumberBucketMatchers(t *testing.T) {
	t.Parallel()
	for expr, want := range map[string]int{
		`rate(vmafx_quality_score_bucket{le="70"}[5m])`:                   1,
		`rate(x_bucket{le = "30", job="a"}[5m]) + y{quantile!="1"}`:       2,
		`rate(vmafx_quality_score_bucket{` + LeMatcher("70") + `}[5m])`:   0,
		`rate(vmafx_server_score_duration_seconds_bucket{le="0.05"}[5m])`: 0,
		`rate(x_bucket{le="+Inf"}[5m])`:                                   0,
	} {
		if got := CheckBucketMatchers(expr); len(got) != want {
			t.Errorf("CheckBucketMatchers(%q) = %v, want %d problems", expr, got, want)
		}
	}
	planted := []byte(`{"title":"T","panels":[{"title":"P","targets":[{"expr":"rate(vmafx_quality_score_bucket{le=\"70\"}[5m])"}]}]}`)
	if problems, err := CheckDashboard(planted); err != nil || len(problems) != 1 {
		t.Errorf("CheckDashboard(le=\"70\") = %v, %v; want one problem", problems, err)
	}
}

// TestLeMatcherSelectsOneBucket: for every bucket of every histogram family,
// LeMatcher's regular expression (as PromQL unquotes it) matches the bound in
// both stored forms and no other bucket's bound in either form.
func TestLeMatcherSelectsOneBucket(t *testing.T) {
	t.Parallel()
	stored := func(b float64) []string {
		s := bucketBound(b)
		if strings.ContainsAny(s, ".e") {
			return []string{s}
		}
		return []string{s, s + ".0"}
	}
	for _, f := range metricdef.All() {
		for _, b := range f.Buckets {
			matcher := LeMatcher(bucketBound(b))
			pattern := strings.ReplaceAll(strings.TrimSuffix(strings.TrimPrefix(matcher, `le=~"`), `"`), `\\`, `\`)
			re := regexp.MustCompile("^(?:" + pattern + ")$")
			for _, other := range f.Buckets {
				for _, v := range stored(other) {
					if re.MatchString(v) != (other == b) {
						t.Errorf("%s: %s on le=%q: match %v", f.Name, matcher, v, re.MatchString(v))
					}
				}
			}
		}
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

// TestEveryRuleQueryIsEmitted: no rule of the generated rule file names a
// series nothing emits, and a planted dead series is reported.
func TestEveryRuleQueryIsEmitted(t *testing.T) {
	t.Parallel()
	raw, err := os.ReadFile(filepath.Join(repoRoot, filepath.FromSlash(RulesFile)))
	if err != nil {
		t.Fatal(err)
	}
	problems, err := CheckRules(raw)
	if err != nil || len(problems) != 0 {
		t.Fatalf("CheckRules = %v, %v", problems, err)
	}
	planted := []byte("groups:\n- name: g\n  rules:\n  - alert: A\n    expr: vmafx_jobs_in_flight > 0\n")
	if problems, err := CheckRules(planted); err != nil || len(problems) != 1 {
		t.Errorf("a dead series in a rule: %v, %v", problems, err)
	}
	planted = []byte("groups:\n- name: g\n  rules:\n  - record: r\n    expr: rate(vmafx_server_score_duration_seconds_bucket{le=\"30\"}[5m])\n")
	if problems, err := CheckRules(planted); err != nil || len(problems) != 1 {
		t.Errorf("a whole-number le matcher in a rule: %v, %v", problems, err)
	}
}

// TestEveryAlertHasARunbook: each alert links a runbook page that exists,
// with one test where it fires and one where it does not, and every runbook
// page belongs to an alert.
func TestEveryAlertHasARunbook(t *testing.T) {
	t.Parallel()
	dir := filepath.Join(repoRoot, "docs", "observability", "runbooks")
	pages := map[string]bool{}
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		if name, ok := strings.CutSuffix(e.Name(), ".md"); ok && name != "index" {
			pages[name] = true
		}
	}
	for _, a := range alerts() {
		if !pages[a.runbook] {
			t.Errorf("%s: no runbook page %s.md", a.name, a.runbook)
		}
		delete(pages, a.runbook)
		fires, quiet := false, false
		for _, c := range a.cases {
			fires = fires || len(c.firing) > 0
			quiet = quiet || len(c.firing) == 0
		}
		if !fires || !quiet {
			t.Errorf("%s: needs a firing and a non-firing case (fires %v, quiet %v)", a.name, fires, quiet)
		}
	}
	for page := range pages {
		t.Errorf("runbook %s.md belongs to no alert", page)
	}
}
