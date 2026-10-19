// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

// Package metricdef is the one definition of every Prometheus metric family
// the VMAFx services expose on /metrics. The services build their collectors
// from it (pkg/observability), the dashboard generator builds its queries from
// it (tools/obsgen), and the metric reference page is generated from it, so a
// name, a label or a bucket layout is written down once (HISS-19).
//
// The package holds data only: it imports nothing beyond the standard library,
// so the generator and the docs pipeline read it without linking a metrics
// client.
package metricdef

import (
	"slices"
	"strings"
)

// Kind is the Prometheus metric type of a family.
type Kind string

const (
	// Counter only goes up; queries take rate() or increase() of it.
	Counter Kind = "counter"
	// Gauge is a value that goes up and down.
	Gauge Kind = "gauge"
	// Histogram counts observations into the family's buckets.
	Histogram Kind = "histogram"
)

// Component names one VMAFx binary that serves /metrics.
type Component string

const (
	// Server is cmd/vmafx-server.
	Server Component = "vmafx-server"
	// Controller is cmd/vmafx-controller.
	Controller Component = "vmafx-controller"
	// Node is cmd/vmafx-node.
	Node Component = "vmafx-node"
)

// Components lists every component that serves /metrics, in a fixed order.
func Components() []Component {
	return []Component{Server, Controller, Node}
}

// Overflow is the value a bounded label takes once its limit of distinct
// values is reached, and the value any label takes for a value outside its
// closed set. A series under it is never dropped, only merged.
const Overflow = "other"

// Label is one label of a family and the bound on its cardinality.
type Label struct {
	// Name is the Prometheus label name.
	Name string
	// Values is the closed value set. Empty means the set is open and Limit
	// bounds it.
	Values []string
	// Limit is the number of distinct values one process reports for an open
	// label; later values are reported as Overflow. Zero for a closed label.
	Limit int
	// Doc says where the value comes from, for the reference page.
	Doc string
}

// MaxValues is the number of distinct values the label can take in one
// process, Overflow included.
func (l Label) MaxValues() int {
	if len(l.Values) > 0 {
		return len(l.Values) + 1
	}
	return l.Limit + 1
}

// Allows reports whether v is one of the closed values of the label. An open
// label allows every value.
func (l Label) Allows(v string) bool {
	return len(l.Values) == 0 || slices.Contains(l.Values, v)
}

// Family is one metric family: its name, type, unit, help text, labels and,
// for a histogram, its bucket upper bounds.
type Family struct {
	// Name is the full Prometheus name, e.g. vmafx_controller_jobs_pending.
	Name string
	// Kind is counter, gauge or histogram.
	Kind Kind
	// Unit is the unit of a sample: seconds, jobs, nodes, requests, score,
	// slots or info.
	Unit string
	// Help is the HELP text on /metrics.
	Help string
	// Labels are the variable labels, in order.
	Labels []Label
	// Buckets are the histogram upper bounds; nil for other kinds.
	Buckets []float64
	// Emitters are the components that register the family.
	Emitters []Component
	// Scraped is true when the value is read from the service's state when
	// Prometheus scrapes, rather than updated as events happen.
	Scraped bool
	// MergeMax is true for a scraped family whose values combine by maximum,
	// not by sum, when bounding maps several of them onto one series (an age:
	// the oldest job of the tenants under Overflow is the oldest of them).
	MergeMax bool
}

// LabelNames returns the family's label names in order.
func (f Family) LabelNames() []string {
	names := make([]string, 0, len(f.Labels))
	for _, l := range f.Labels {
		names = append(names, l.Name)
	}
	return names
}

// EmittedBy reports whether c registers the family.
func (f Family) EmittedBy(c Component) bool {
	return slices.Contains(f.Emitters, c)
}

// SeriesNames returns every series name the family puts on /metrics: the name
// itself, and for a histogram its _bucket, _sum and _count series.
func (f Family) SeriesNames() []string {
	if f.Kind != Histogram {
		return []string{f.Name}
	}
	return []string{f.Name + "_bucket", f.Name + "_sum", f.Name + "_count"}
}

// MaxSeries is the largest number of series the family can put on /metrics of
// one process: the product of its label bounds, times the bucket series plus
// _sum and _count for a histogram.
func (f Family) MaxSeries() int {
	n := 1
	for _, l := range f.Labels {
		n *= l.MaxValues()
	}
	if f.Kind == Histogram {
		// One _bucket series per bound plus +Inf, then _sum and _count.
		n *= len(f.Buckets) + 3
	}
	return n
}

// ByEmitter returns the families c registers, in definition order.
func ByEmitter(c Component) []Family {
	var out []Family
	for _, f := range All() {
		if f.EmittedBy(c) {
			out = append(out, f)
		}
	}
	return out
}

// Lookup returns the family that puts series name on /metrics, and whether
// there is one.
func Lookup(series string) (Family, bool) {
	for _, f := range All() {
		if slices.Contains(f.SeriesNames(), series) {
			return f, true
		}
	}
	return Family{}, false
}

// IsVMAFx reports whether a series name is in the vmafx_ namespace.
func IsVMAFx(series string) bool {
	return strings.HasPrefix(series, "vmafx_")
}
