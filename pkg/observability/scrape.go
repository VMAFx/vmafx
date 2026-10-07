// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package observability

import (
	"context"
	"fmt"
	"strings"
	"time"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// ScrapeTimeout bounds one read of a service's state for the families
// metricdef marks Scraped (HISS-02). A read that takes longer fails the
// families of that read for this scrape; the next scrape tries again.
const ScrapeTimeout = 2 * time.Second

// Sample is one value of a scraped family: the family, the value and its
// label values in the family's label order.
type Sample struct {
	Family metricdef.Family
	Value  float64
	Labels []string
}

// ScrapeFunc reads the current values of a group of scraped families. It is
// called once per Prometheus scrape with a context bounded by ScrapeTimeout.
type ScrapeFunc func(ctx context.Context) ([]Sample, error)

// RegisterScraped registers families, all marked Scraped in metricdef, as one
// collector on reg whose values come from fn when Prometheus scrapes. Label
// values are bounded as for the event-driven handles.
func RegisterScraped(reg prometheus.Registerer, families []metricdef.Family, fn ScrapeFunc) error {
	c := &scrapeCollector{
		fn: fn, descs: map[string]*prometheus.Desc{}, bounds: map[string]*bounder{},
		kinds: map[string]prometheus.ValueType{},
	}
	for _, f := range families {
		if !f.Scraped {
			return fmt.Errorf("observability: %s is not a scraped family", f.Name)
		}
		c.order = append(c.order, f.Name)
		c.descs[f.Name] = prometheus.NewDesc(f.Name, f.Help, f.LabelNames(), nil)
		c.bounds[f.Name] = newBounder(f)
		c.kinds[f.Name] = valueType(f.Kind)
	}
	if err := reg.Register(c); err != nil {
		return fmt.Errorf("observability: register scraped families: %w", err)
	}
	return nil
}

func valueType(k metricdef.Kind) prometheus.ValueType {
	if k == metricdef.Counter {
		return prometheus.CounterValue
	}
	return prometheus.GaugeValue
}

type scrapeCollector struct {
	fn     ScrapeFunc
	order  []string
	kinds  map[string]prometheus.ValueType
	descs  map[string]*prometheus.Desc
	bounds map[string]*bounder
}

func (c *scrapeCollector) Describe(ch chan<- *prometheus.Desc) {
	for _, name := range c.order {
		ch <- c.descs[name]
	}
}

func (c *scrapeCollector) Collect(ch chan<- prometheus.Metric) {
	ctx, cancel := context.WithTimeout(context.Background(), ScrapeTimeout)
	defer cancel()
	samples, err := c.fn(ctx)
	if err != nil {
		for _, name := range c.order {
			ch <- prometheus.NewInvalidMetric(c.descs[name], err)
		}
		return
	}
	sums := c.merge(samples)
	for _, s := range sums {
		ch <- s
	}
}

// merge turns samples into const metrics, adding the values of samples that
// bounding mapped onto the same series (two tenants past the limit become one
// "other" series whose value is their sum).
func (c *scrapeCollector) merge(samples []Sample) []prometheus.Metric {
	type key struct{ name, labels string }
	values := map[key]float64{}
	labelsOf := map[key][]string{}
	var keys []key
	for _, s := range samples {
		desc, ok := c.descs[s.Family.Name]
		if !ok || desc == nil {
			continue
		}
		lv := c.bounds[s.Family.Name].values(s.Labels)
		k := key{s.Family.Name, strings.Join(lv, "\xff")}
		_, seen := values[k]
		if !seen {
			keys = append(keys, k)
			labelsOf[k] = lv
		}
		values[k] = combine(s.Family, values[k], s.Value, !seen)
	}
	out := make([]prometheus.Metric, 0, len(keys))
	for _, k := range keys {
		m, err := prometheus.NewConstMetric(c.descs[k.name], c.kinds[k.name], values[k], labelsOf[k]...)
		if err != nil {
			m = prometheus.NewInvalidMetric(c.descs[k.name], err)
		}
		out = append(out, m)
	}
	return out
}

// combine folds v into acc for a series several samples map onto: the larger
// value for a family metricdef marks MergeMax (an age), the sum otherwise.
func combine(f metricdef.Family, acc, v float64, first bool) float64 {
	switch {
	case first:
		return v
	case f.MergeMax:
		return max(acc, v)
	default:
		return acc + v
	}
}
