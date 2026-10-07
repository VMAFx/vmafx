// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package observability

import (
	"context"
	"fmt"
	"net/http"
	"sync"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"go.opentelemetry.io/otel/trace"

	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// Counter, Gauge and Histogram are handles on one family of metricdef. They
// bound every label value before it reaches the client library (see bounder),
// so a caller cannot grow a family past the cardinality metricdef states.
type (
	// Counter is a counter family.
	Counter struct {
		vec   *prometheus.CounterVec
		bound *bounder
	}
	// Gauge is a gauge family.
	Gauge struct {
		vec   *prometheus.GaugeVec
		bound *bounder
	}
	// Histogram is a histogram family.
	Histogram struct {
		vec   *prometheus.HistogramVec
		bound *bounder
	}
)

// Inc adds 1 to the series of labels.
func (c Counter) Inc(labels ...string) { c.Add(1, labels...) }

// Add adds v to the series of labels.
func (c Counter) Add(v float64, labels ...string) {
	if c.vec != nil {
		c.vec.WithLabelValues(c.bound.values(labels)...).Add(v)
	}
}

// Set sets the series of labels to v.
func (g Gauge) Set(v float64, labels ...string) {
	if g.vec != nil {
		g.vec.WithLabelValues(g.bound.values(labels)...).Set(v)
	}
}

// Add adds v (negative to subtract) to the series of labels.
func (g Gauge) Add(v float64, labels ...string) {
	if g.vec != nil {
		g.vec.WithLabelValues(g.bound.values(labels)...).Add(v)
	}
}

// Observe records v in the series of labels.
func (h Histogram) Observe(v float64, labels ...string) {
	if h.vec != nil {
		h.vec.WithLabelValues(h.bound.values(labels)...).Observe(v)
	}
}

// ObserveContext records v like Observe and, when ctx carries a sampled span,
// attaches the span's trace id as an exemplar (trace_id), so a latency panel
// opens the trace of a slow request. /metrics serves exemplars in the
// OpenMetrics format (MetricsHandler).
func (h Histogram) ObserveContext(ctx context.Context, v float64, labels ...string) {
	if h.vec == nil {
		return
	}
	o := h.vec.WithLabelValues(h.bound.values(labels)...)
	sc := trace.SpanContextFromContext(ctx)
	eo, ok := o.(prometheus.ExemplarObserver)
	if !ok || !sc.IsSampled() {
		o.Observe(v)
		return
	}
	eo.ObserveWithExemplar(v, prometheus.Labels{ExemplarTraceID: sc.TraceID().String()})
}

// ExemplarTraceID is the exemplar label that holds the trace id; Grafana's
// exemplar link to the trace backend matches on it.
const ExemplarTraceID = "trace_id"

// MetricsHandler serves reg on /metrics, in the OpenMetrics format when the
// scraper asks for it, so exemplars reach Prometheus (ADR-1014: every service
// serves its own registry).
func MetricsHandler(reg *prometheus.Registry) http.Handler {
	return promhttp.HandlerFor(reg, promhttp.HandlerOpts{EnableOpenMetrics: true})
}

// NewCounter registers f, a counter family, on reg.
func NewCounter(reg prometheus.Registerer, f metricdef.Family) (Counter, error) {
	if err := checkKind(f, metricdef.Counter); err != nil {
		return Counter{}, err
	}
	vec := prometheus.NewCounterVec(prometheus.CounterOpts{Name: f.Name, Help: f.Help}, f.LabelNames())
	if err := reg.Register(vec); err != nil {
		return Counter{}, fmt.Errorf("observability: register %s: %w", f.Name, err)
	}
	if len(f.Labels) == 0 {
		vec.WithLabelValues() // an unlabelled family serves its series from the start, at 0
	}
	return Counter{vec: vec, bound: newBounder(f)}, nil
}

// NewGauge registers f, a gauge family that is set as events happen, on reg.
// A family metricdef marks Scraped is registered with RegisterScraped instead.
func NewGauge(reg prometheus.Registerer, f metricdef.Family) (Gauge, error) {
	if err := checkKind(f, metricdef.Gauge); err != nil {
		return Gauge{}, err
	}
	if f.Scraped {
		return Gauge{}, fmt.Errorf("observability: %s is read at scrape time; use RegisterScraped", f.Name)
	}
	vec := prometheus.NewGaugeVec(prometheus.GaugeOpts{Name: f.Name, Help: f.Help}, f.LabelNames())
	if err := reg.Register(vec); err != nil {
		return Gauge{}, fmt.Errorf("observability: register %s: %w", f.Name, err)
	}
	if len(f.Labels) == 0 {
		vec.WithLabelValues()
	}
	return Gauge{vec: vec, bound: newBounder(f)}, nil
}

// NewHistogram registers f, a histogram family, on reg with its buckets.
func NewHistogram(reg prometheus.Registerer, f metricdef.Family) (Histogram, error) {
	if err := checkKind(f, metricdef.Histogram); err != nil {
		return Histogram{}, err
	}
	vec := prometheus.NewHistogramVec(prometheus.HistogramOpts{
		Name: f.Name, Help: f.Help, Buckets: f.Buckets,
	}, f.LabelNames())
	if err := reg.Register(vec); err != nil {
		return Histogram{}, fmt.Errorf("observability: register %s: %w", f.Name, err)
	}
	if len(f.Labels) == 0 {
		vec.WithLabelValues()
	}
	return Histogram{vec: vec, bound: newBounder(f)}, nil
}

func checkKind(f metricdef.Family, want metricdef.Kind) error {
	if f.Kind != want {
		return fmt.Errorf("observability: %s is a %s, not a %s", f.Name, f.Kind, want)
	}
	return nil
}

// bounder maps the label values of one family onto the values metricdef
// allows: an empty value becomes metricdef.None, a value outside a closed set
// becomes metricdef.Overflow, and an open label reports at most its Limit
// distinct values before it reports metricdef.Overflow for every new one.
type bounder struct {
	labels []metricdef.Label
	mu     sync.Mutex
	seen   []map[string]struct{}
}

func newBounder(f metricdef.Family) *bounder {
	b := &bounder{labels: f.Labels, seen: make([]map[string]struct{}, len(f.Labels))}
	for i := range b.seen {
		b.seen[i] = make(map[string]struct{})
	}
	return b
}

// values returns exactly one bounded value per label of the family: missing
// values are metricdef.None, extra values are ignored.
func (b *bounder) values(in []string) []string {
	out := make([]string, len(b.labels))
	b.mu.Lock()
	defer b.mu.Unlock()
	for i, l := range b.labels {
		v := ""
		if i < len(in) {
			v = in[i]
		}
		out[i] = b.bound(i, l, v)
	}
	return out
}

func (b *bounder) bound(i int, l metricdef.Label, v string) string {
	if v == "" {
		v = metricdef.None
	}
	if len(l.Values) > 0 {
		if l.Allows(v) {
			return v
		}
		return metricdef.Overflow
	}
	if _, ok := b.seen[i][v]; ok {
		return v
	}
	if len(b.seen[i]) >= l.Limit {
		return metricdef.Overflow
	}
	b.seen[i][v] = struct{}{}
	return v
}
