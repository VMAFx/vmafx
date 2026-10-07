// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package observability

import (
	"context"
	"errors"
	"fmt"
	"testing"

	"github.com/prometheus/client_golang/prometheus"
	dto "github.com/prometheus/client_model/go"

	"github.com/VMAFx/vmafx/pkg/model"
	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// seriesOf gathers reg and returns the label sets and values of family name,
// keyed by the label values joined with "|".
func seriesOf(t *testing.T, reg *prometheus.Registry, name string) map[string]float64 {
	t.Helper()
	fams, err := reg.Gather()
	if err != nil {
		t.Fatalf("Gather: %v", err)
	}
	out := map[string]float64{}
	for _, f := range fams {
		if f.GetName() != name {
			continue
		}
		for _, m := range f.GetMetric() {
			out[labelKey(m)] = metricValue(m)
		}
	}
	return out
}

func labelKey(m *dto.Metric) string {
	k := ""
	for i, lp := range m.GetLabel() {
		if i > 0 {
			k += "|"
		}
		k += lp.GetValue()
	}
	return k
}

func metricValue(m *dto.Metric) float64 {
	switch {
	case m.GetCounter() != nil:
		return m.GetCounter().GetValue()
	case m.GetGauge() != nil:
		return m.GetGauge().GetValue()
	case m.GetHistogram() != nil:
		return float64(m.GetHistogram().GetSampleCount())
	}
	return 0
}

func TestBounderClosedSetEmptyAndOverflow(t *testing.T) {
	t.Parallel()
	reg := prometheus.NewRegistry()
	c, err := NewCounter(reg, metricdef.NodeJobs)
	if err != nil {
		t.Fatal(err)
	}
	c.Inc("cuda", "completed")
	c.Inc("", "failed")          // no backend -> none
	c.Inc("opencl", "completed") // outside the closed set -> other
	c.Inc("cuda", "exploded")    // outside the closed set -> other
	c.Inc("cpu")                 // missing value -> none, then other for outcome
	got := seriesOf(t, reg, metricdef.NodeJobs.Name)
	want := map[string]float64{
		"cuda|completed":  1,
		"none|failed":     1,
		"other|completed": 1,
		"cuda|other":      1,
		"cpu|other":       1,
	}
	if fmt.Sprint(got) != fmt.Sprint(want) {
		t.Errorf("series = %v, want %v", got, want)
	}
}

func TestBounderOpenLabelLimit(t *testing.T) {
	t.Parallel()
	reg := prometheus.NewRegistry()
	c, err := NewCounter(reg, metricdef.ControllerJobsSubmitted)
	if err != nil {
		t.Fatal(err)
	}
	for i := range metricdef.TenantLimit + 5 {
		c.Inc(fmt.Sprintf("t%02d", i))
	}
	c.Inc("t00") // a value seen before the limit keeps its series
	got := seriesOf(t, reg, metricdef.ControllerJobsSubmitted.Name)
	if len(got) != metricdef.TenantLimit+1 {
		t.Fatalf("got %d series, want %d", len(got), metricdef.TenantLimit+1)
	}
	if got["other"] != 5 || got["t00"] != 2 {
		t.Errorf("other = %v (want 5), t00 = %v (want 2)", got["other"], got["t00"])
	}
}

func TestHandlesRefuseTheWrongKind(t *testing.T) {
	t.Parallel()
	reg := prometheus.NewRegistry()
	if _, err := NewCounter(reg, metricdef.NodeSlots); err == nil {
		t.Error("NewCounter accepted a gauge family")
	}
	if _, err := NewHistogram(reg, metricdef.NodeJobs); err == nil {
		t.Error("NewHistogram accepted a counter family")
	}
	if _, err := NewGauge(reg, metricdef.ControllerJobsPending); err == nil {
		t.Error("NewGauge accepted a scraped family")
	}
	if _, err := NewGauge(reg, metricdef.NodeSlots); err != nil {
		t.Fatalf("NewGauge: %v", err)
	}
	if _, err := NewGauge(reg, metricdef.NodeSlots); err == nil {
		t.Error("a second registration of one family succeeded")
	}
}

func TestZeroHandlesAreNoOps(t *testing.T) {
	t.Parallel()
	var c Counter
	var g Gauge
	var h Histogram
	c.Inc("x")
	g.Set(1)
	g.Add(1)
	h.Observe(1)
}

func TestRegisterScrapedValuesAndMerging(t *testing.T) {
	t.Parallel()
	reg := prometheus.NewRegistry()
	fams := []metricdef.Family{metricdef.ControllerJobsPending, metricdef.ControllerQueueOldestAge}
	var samples []Sample
	for i := range metricdef.TenantLimit + 2 {
		tenant := fmt.Sprintf("t%02d", i)
		samples = append(samples,
			Sample{Family: metricdef.ControllerJobsPending, Value: 1, Labels: []string{tenant}},
			Sample{Family: metricdef.ControllerQueueOldestAge, Value: float64(10 * (i + 1)), Labels: []string{tenant}})
	}
	err := RegisterScraped(reg, readErrors(t, reg), ScrapeGroup{Source: "queue", Families: fams,
		Read: func(context.Context) ([]Sample, error) { return samples, nil }})
	if err != nil {
		t.Fatal(err)
	}
	pending := seriesOf(t, reg, metricdef.ControllerJobsPending.Name)
	age := seriesOf(t, reg, metricdef.ControllerQueueOldestAge.Name)
	// The two tenants past the limit share "other": counts add, ages take the
	// larger value.
	if pending["other"] != 2 || pending["t00"] != 1 {
		t.Errorf("pending other = %v (want 2), t00 = %v (want 1)", pending["other"], pending["t00"])
	}
	if want := float64(10 * (metricdef.TenantLimit + 2)); age["other"] != want {
		t.Errorf("age other = %v, want %v", age["other"], want)
	}
}

func TestRegisterScrapedRefusesEventFamilies(t *testing.T) {
	t.Parallel()
	reg := prometheus.NewRegistry()
	err := RegisterScraped(reg, readErrors(t, reg), ScrapeGroup{Source: "queue",
		Families: []metricdef.Family{metricdef.NodeSlots},
		Read:     func(context.Context) ([]Sample, error) { return nil, nil }})
	if err == nil {
		t.Fatal("RegisterScraped accepted an event-driven family")
	}
}

// readErrors registers the read-error family on reg.
func readErrors(t *testing.T, reg prometheus.Registerer) Counter {
	t.Helper()
	errs, err := NewReadErrors(reg)
	if err != nil {
		t.Fatal(err)
	}
	return errs
}

// TestRegisterScrapedCountsReadErrors: a failed read counts under its source
// and leaves only its own families out; the rest of the page is served.
func TestRegisterScrapedCountsReadErrors(t *testing.T) {
	t.Parallel()
	reg := prometheus.NewRegistry()
	errs := readErrors(t, reg)
	failing := ScrapeGroup{Source: "device_memory", Families: []metricdef.Family{metricdef.NodeDeviceMemoryUsed},
		Read: func(context.Context) ([]Sample, error) { return nil, errors.New("nvidia-smi: not found") }}
	working := ScrapeGroup{Source: "queue", Families: []metricdef.Family{metricdef.ControllerNodesLive},
		Read: func(context.Context) ([]Sample, error) {
			return []Sample{{Family: metricdef.ControllerNodesLive, Value: 2}}, nil
		}}
	for _, g := range []ScrapeGroup{failing, working} {
		if err := RegisterScraped(reg, errs, g); err != nil {
			t.Fatal(err)
		}
	}
	got := seriesOf(t, reg, metricdef.MetricsReadErrors.Name) // Gather must not fail
	if got["device_memory"] != 1 || got["queue"] != 0 {
		t.Errorf("read errors = %v, want device_memory 1, queue 0", got)
	}
	if nodes := seriesOf(t, reg, metricdef.ControllerNodesLive.Name); nodes[""] != 2 {
		t.Errorf("the working group was not served: %v", nodes)
	}
	if mem := seriesOf(t, reg, metricdef.NodeDeviceMemoryUsed.Name); len(mem) != 0 {
		t.Errorf("the failed group served %v", mem)
	}
}

// TestRegisterScrapedPassesABoundedContext checks the read gets a deadline
// (HISS-02).
func TestRegisterScrapedPassesABoundedContext(t *testing.T) {
	t.Parallel()
	reg := prometheus.NewRegistry()
	hasDeadline := false
	err := RegisterScraped(reg, readErrors(t, reg), ScrapeGroup{Source: "queue",
		Families: []metricdef.Family{metricdef.ControllerNodesLive},
		Read: func(ctx context.Context) ([]Sample, error) {
			_, hasDeadline = ctx.Deadline()
			return []Sample{{Family: metricdef.ControllerNodesLive, Value: 3}}, nil
		}})
	if err != nil {
		t.Fatal(err)
	}
	if got := seriesOf(t, reg, metricdef.ControllerNodesLive.Name); got[""] != 3 || !hasDeadline {
		t.Errorf("nodes_live = %v, deadline = %v", got, hasDeadline)
	}
}

// TestObserveScoreLabels: a score from vmafx-server (no tenant) and a request
// without a model land under tenant none, the default model and profile none
// (decision Q-119: no request carries a profile yet).
func TestObserveScoreLabels(t *testing.T) {
	t.Parallel()
	reg := prometheus.NewRegistry()
	m, err := NewMetrics(reg)
	if err != nil {
		t.Fatal(err)
	}
	m.ObserveScore("", "", 91)
	m.ObserveScore("acme", "vmaf_v0.6.1", 88)
	got := seriesOf(t, reg, metricdef.QualityScore.Name)
	want := map[string]float64{
		model.DefaultVersion + "|none|none": 1,
		"vmaf_v0.6.1|none|acme":             1,
	}
	if fmt.Sprint(got) != fmt.Sprint(want) {
		t.Errorf("quality series = %v, want %v", got, want)
	}
}
