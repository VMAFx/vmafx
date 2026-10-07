// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package metricdef

import (
	"regexp"
	"slices"
	"strings"
	"testing"
)

// seriesBudget is the largest number of series one family may put on the
// /metrics page of one process (docs/observability/metrics.md).
const seriesBudget = 11000

var nameRe = regexp.MustCompile(`^vmafx_[a-z][a-z0-9_]*[a-z0-9]$`)

// check is one rule a family must keep: bad reports a breach, msg names it.
type check struct {
	bad func(Family) bool
	msg string
}

// nameChecks are the naming rules.
var nameChecks = []check{
	{func(f Family) bool { return !nameRe.MatchString(f.Name) }, "name is not vmafx_ snake case"},
	{func(f Family) bool { return f.Help == "" || f.Unit == "" }, "help or unit missing"},
	{func(f Family) bool { return len(f.Emitters) == 0 }, "no component emits it"},
	{func(f Family) bool { return (f.Kind == Counter) != strings.HasSuffix(f.Name, "_total") }, "counters, and only counters, end in _total"},
	{func(f Family) bool { return f.Unit == "seconds" && !strings.HasSuffix(f.Name, "_seconds") }, "seconds family does not end in _seconds"},
}

// shapeChecks are the type and cardinality rules.
var shapeChecks = []check{
	{func(f Family) bool { return f.Kind == Histogram && !slices.IsSorted(f.Buckets) }, "histogram buckets unsorted"},
	{func(f Family) bool { return (f.Kind == Histogram) != (len(f.Buckets) > 0) }, "buckets on a non-histogram, or a histogram without them"},
	{func(f Family) bool { return f.Scraped && f.Kind == Histogram }, "a histogram cannot be scraped"},
	{func(f Family) bool { return f.MergeMax && !f.Scraped }, "MergeMax on an event-driven family"},
	{func(f Family) bool { return f.MaxSeries() > seriesBudget }, "more series than the per-family budget"},
	{func(f Family) bool { return hasDuplicateLabel(f) }, "a label appears twice"},
	{func(f Family) bool { return hasBadLabel(f) }, "a label lacks a doc, or does not set exactly one of Values and Limit"},
}

// problems returns every rule f breaks; an empty slice is a valid family.
func problems(f Family) []string {
	var out []string
	for _, c := range slices.Concat(nameChecks, shapeChecks) {
		if c.bad(f) {
			out = append(out, f.Name+": "+c.msg)
		}
	}
	return out
}

func hasDuplicateLabel(f Family) bool {
	names := slices.Clone(f.LabelNames())
	slices.Sort(names)
	return len(slices.Compact(names)) != len(f.Labels)
}

func hasBadLabel(f Family) bool {
	return slices.ContainsFunc(f.Labels, func(l Label) bool {
		return l.Doc == "" || (len(l.Values) == 0) == (l.Limit == 0)
	})
}

func TestEveryFamilyFollowsTheNamingAndCardinalityRules(t *testing.T) {
	t.Parallel()
	seen := map[string]bool{}
	for _, f := range All() {
		for _, p := range problems(f) {
			t.Error(p)
		}
		for _, s := range f.SeriesNames() {
			if seen[s] {
				t.Errorf("series %s is defined twice", s)
			}
			seen[s] = true
		}
	}
}

// TestRulesRefuseBrokenFamilies is the negative case: each planted defect is
// reported by problems().
func TestRulesRefuseBrokenFamilies(t *testing.T) {
	t.Parallel()
	ok := Family{Name: "vmafx_x_total", Kind: Counter, Unit: "jobs", Help: "h", Emitters: []Component{Node}}
	if p := problems(ok); len(p) != 0 {
		t.Fatalf("valid family refused: %v", p)
	}
	broken := map[string]Family{
		"no _total":     {Name: "vmafx_x", Kind: Counter, Unit: "jobs", Help: "h", Emitters: []Component{Node}},
		"no emitter":    {Name: "vmafx_x_total", Kind: Counter, Unit: "jobs", Help: "h"},
		"bad name":      {Name: "x_total", Kind: Counter, Unit: "jobs", Help: "h", Emitters: []Component{Node}},
		"no buckets":    {Name: "vmafx_x_seconds", Kind: Histogram, Unit: "seconds", Help: "h", Emitters: []Component{Node}},
		"open no limit": {Name: "vmafx_x_total", Kind: Counter, Unit: "jobs", Help: "h", Emitters: []Component{Node}, Labels: []Label{{Name: "a", Doc: "d"}}},
		"too many series": {Name: "vmafx_x_total", Kind: Counter, Unit: "jobs", Help: "h", Emitters: []Component{Node},
			Labels: []Label{{Name: "a", Limit: 200, Doc: "d"}, {Name: "b", Limit: 200, Doc: "d"}}},
	}
	for name, f := range broken {
		if len(problems(f)) == 0 {
			t.Errorf("%s: planted defect not reported", name)
		}
	}
}

func TestSeriesNamesAndLookup(t *testing.T) {
	t.Parallel()
	got := ControllerJobQueueWait.SeriesNames()
	want := []string{
		"vmafx_controller_job_queue_wait_seconds_bucket",
		"vmafx_controller_job_queue_wait_seconds_sum",
		"vmafx_controller_job_queue_wait_seconds_count",
	}
	if !slices.Equal(got, want) {
		t.Fatalf("SeriesNames = %v, want %v", got, want)
	}
	if f, ok := Lookup("vmafx_controller_job_queue_wait_seconds_bucket"); !ok || f.Name != ControllerJobQueueWait.Name {
		t.Errorf("Lookup of a bucket series = %v, %v", f.Name, ok)
	}
	if _, ok := Lookup("vmafx_controller_job_queue_wait_seconds"); ok {
		t.Error("the bare name of a histogram is not a series and must not resolve")
	}
	if _, ok := Lookup("vmafx_controller_jobs_queued"); ok {
		t.Error("a name nothing defines resolved")
	}
}

func TestByEmitterPartitionsTheFamilies(t *testing.T) {
	t.Parallel()
	n := 0
	for _, c := range Components() {
		fams := ByEmitter(c)
		if len(fams) == 0 {
			t.Errorf("%s emits nothing", c)
		}
		n += len(fams)
	}
	shared := 0
	for _, f := range All() {
		shared += len(f.Emitters) - 1
	}
	if n != len(All())+shared {
		t.Errorf("emitter lists do not add up: %d vs %d", n, len(All())+shared)
	}
}

func TestMaxSeriesOfTheQualityFamily(t *testing.T) {
	t.Parallel()
	// (32 tenants + other) x (16 models + other) x (16 buckets + +Inf + _sum + _count)
	if got, want := QualityScore.MaxSeries(), 33*17*19; got != want {
		t.Errorf("MaxSeries = %d, want %d", got, want)
	}
}
