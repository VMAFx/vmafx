// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"reflect"
	"regexp"
	"slices"

	"go.yaml.in/yaml/v3"

	m "github.com/VMAFx/vmafx/pkg/observability/metricdef"
)

// Settings are the tunable SLO objectives, burn-rate windows and alert
// thresholds (decision Q-165). They are the Helm values monitoring.slo,
// monitoring.burnRates and monitoring.alerts; the chart's PrometheusRule
// reads them when it renders, and RenderRules renders a plain rule file from
// a values file for the Compose example.
type Settings struct {
	SLO       SLOSettings   `yaml:"slo"`
	BurnRates BurnRates     `yaml:"burnRates"`
	Alerts    AlertSettings `yaml:"alerts"`
	Cost      CostSettings  `yaml:"cost"`
}

// CostSettings are the prices of the usage and cost dashboard (decision
// Q-193): one price per second of job run time and one per job, the same for
// every backend, in Currency. A price of 0 (the default) is unset: its cost
// panels stay empty, never filled with an assumed price.
type CostSettings struct {
	// PerJobSecond is the price of one second a node spends on a job.
	PerJobSecond float64 `yaml:"perJobSecond"`
	// PerJob is the price of one completed or failed job; a cancelled job is
	// not charged (Q-208).
	PerJob float64 `yaml:"perJob"`
	// Currency is the ISO 4217 code the panels show, or empty.
	Currency string `yaml:"currency"`
}

// SLOSettings are the objectives: the share of events that must be good
// over 30 days. The error budget is 1 - objective.
type SLOSettings struct {
	// JobSuccess: controller jobs that complete rather than fail.
	JobSuccess float64 `yaml:"jobSuccess"`
	// ScoreSuccess: Score requests that do not return an error.
	ScoreSuccess float64 `yaml:"scoreSuccess"`
	// ScoreLatency: Score requests that finish within ScoreLatencySeconds.
	ScoreLatency float64 `yaml:"scoreLatency"`
	// ScoreLatencySeconds is a bucket bound of
	// vmafx_server_score_duration_seconds (metricdef.RequestSecondsBuckets).
	ScoreLatencySeconds string `yaml:"scoreLatencySeconds"`
}

// BurnRates are the two burn-rate rules of each SLO (multi-window,
// multi-burn-rate alerting).
type BurnRates struct {
	Fast BurnRate `yaml:"fast"`
	Slow BurnRate `yaml:"slow"`
}

// BurnRate is one rule: the ratio of bad events over the long and the short
// window both above Factor times the error budget, for For.
type BurnRate struct {
	LongWindow  string  `yaml:"longWindow"`
	ShortWindow string  `yaml:"shortWindow"`
	Factor      float64 `yaml:"factor"`
	For         string  `yaml:"for"`
}

// AlertSettings are the thresholds of the alerts that are not burn rates.
type AlertSettings struct {
	// QueueAgeSeconds: how old a tenant's oldest pending job may get.
	QueueAgeSeconds int `yaml:"queueAgeSeconds"`
	// ScoreRegressionPoints: the drop of an hour's median score below the
	// median of the previous day's hourly medians that is reported.
	ScoreRegressionPoints float64 `yaml:"scoreRegressionPoints"`
	// ScoreRegressionMinScores: the scores an hour needs to be compared.
	ScoreRegressionMinScores int `yaml:"scoreRegressionMinScores"`
}

// DefaultSettings are the defaults of the chart and the Compose example: a
// fast burn spends 2 % of a 30-day budget in an hour, a slow one 5 % in six
// hours.
func DefaultSettings() Settings {
	return Settings{
		SLO: SLOSettings{JobSuccess: 0.99, ScoreSuccess: 0.99, ScoreLatency: 0.99, ScoreLatencySeconds: "30"},
		BurnRates: BurnRates{
			Fast: BurnRate{LongWindow: "1h", ShortWindow: "5m", Factor: 14.4, For: "2m"},
			Slow: BurnRate{LongWindow: "6h", ShortWindow: "30m", Factor: 6, For: "15m"},
		},
		Alerts: AlertSettings{QueueAgeSeconds: 1800, ScoreRegressionPoints: 5, ScoreRegressionMinScores: 20},
	}
}

// ApplyValues overlays the monitoring.slo, monitoring.burnRates and
// monitoring.alerts of a Helm values document on s, as Helm merges a values
// file over the chart's: keys the document sets replace, the others stay.
// Other keys of the document are ignored; an unknown key inside those three
// is an error. The result is validated.
func (s Settings) ApplyValues(doc []byte) (Settings, error) {
	var top struct {
		Monitoring map[string]yaml.Node `yaml:"monitoring"`
	}
	if err := yaml.Unmarshal(doc, &top); err != nil {
		return s, fmt.Errorf("obsgen: parse values: %w", err)
	}
	picked := map[string]*yaml.Node{}
	settings := reflect.TypeFor[Settings]()
	for field := range settings.Fields() {
		key := field.Tag.Get("yaml")
		if n, ok := top.Monitoring[key]; ok {
			if err := checkScalarTypes(&n, field.Type, "monitoring."+key); err != nil {
				return s, err
			}
			picked[key] = &n
		}
	}
	raw, err := yaml.Marshal(picked)
	if err != nil {
		return s, fmt.Errorf("obsgen: values: %w", err)
	}
	dec := yaml.NewDecoder(bytes.NewReader(raw))
	dec.KnownFields(true)
	if err := dec.Decode(&s); err != nil && !errors.Is(err, io.EOF) {
		return s, fmt.Errorf("obsgen: monitoring settings: %w", err)
	}
	return s, s.Validate()
}

// scalarCheck is one node of the settings tree still to check.
type scalarCheck struct {
	node *yaml.Node
	typ  reflect.Type
	path string
}

// checkScalarTypes refuses a scalar whose YAML type is not the field's, as
// the chart's schema does: a quoted bound where a number belongs, a number
// where the latency bound's string belongs, a fraction for an integer. The
// decoder alone converts those silently. It walks the tree with a work list
// bounded by the node count (HISS-01: no recursion).
func checkScalarTypes(n *yaml.Node, t reflect.Type, path string) error {
	todo := []scalarCheck{{n, t, path}}
	for len(todo) > 0 {
		c := todo[len(todo)-1]
		todo = todo[:len(todo)-1]
		if c.typ.Kind() != reflect.Struct {
			if err := checkScalar(c); err != nil {
				return err
			}
			continue
		}
		if c.node.Kind != yaml.MappingNode {
			return fmt.Errorf("obsgen: %s must be a mapping", c.path)
		}
		for i := 0; i+1 < len(c.node.Content); i += 2 {
			key := c.node.Content[i].Value
			f, ok := fieldByTag(c.typ, key)
			if !ok {
				return fmt.Errorf("obsgen: %s.%s is no setting", c.path, key)
			}
			todo = append(todo, scalarCheck{c.node.Content[i+1], f.Type, c.path + "." + key})
		}
	}
	return nil
}

// checkScalar refuses a leaf whose YAML tag does not fit its field's kind.
func checkScalar(c scalarCheck) error {
	want := map[reflect.Kind][]string{
		reflect.Float64: {"!!int", "!!float"}, reflect.Int: {"!!int"}, reflect.String: {"!!str"},
	}[c.typ.Kind()]
	if c.node.Kind != yaml.ScalarNode || !slices.Contains(want, c.node.ShortTag()) {
		return fmt.Errorf("obsgen: %s=%q is a %s, want %v", c.path, c.node.Value, c.node.ShortTag(), want)
	}
	return nil
}

// fieldByTag finds the struct field of t with yaml tag key.
func fieldByTag(t reflect.Type, key string) (reflect.StructField, bool) {
	for field := range t.Fields() {
		if field.Tag.Get("yaml") == key {
			return field, true
		}
	}
	return reflect.StructField{}, false
}

// Validate refuses the settings the chart's values.schema.json refuses: an
// objective outside (0, 1), a latency bound that is no bucket of the
// histogram, a window or for that is no Prometheus duration, a factor or
// threshold that is not positive, a regression drop above 100 points.
// TestValidateAgreesWithTheChartSchema holds the two to the same cases.
func (s Settings) Validate() error {
	for name, o := range map[string]float64{
		"slo.jobSuccess": s.SLO.JobSuccess, "slo.scoreSuccess": s.SLO.ScoreSuccess, "slo.scoreLatency": s.SLO.ScoreLatency,
	} {
		if o <= 0 || o >= 1 {
			return fmt.Errorf("obsgen: %s=%g must lie strictly between 0 and 1", name, o)
		}
	}
	if !slices.Contains(LatencyBounds(), s.SLO.ScoreLatencySeconds) {
		return fmt.Errorf("obsgen: slo.scoreLatencySeconds=%q is none of the bucket bounds %v of %s",
			s.SLO.ScoreLatencySeconds, LatencyBounds(), m.ServerScoreDuration.Name)
	}
	for name, b := range map[string]BurnRate{"burnRates.fast": s.BurnRates.Fast, "burnRates.slow": s.BurnRates.Slow} {
		if b.Factor <= 0 {
			return fmt.Errorf("obsgen: %s.factor=%g must be positive", name, b.Factor)
		}
		for key, d := range map[string]string{"longWindow": b.LongWindow, "shortWindow": b.ShortWindow, "for": b.For} {
			if !promDuration.MatchString(d) {
				return fmt.Errorf("obsgen: %s.%s=%q is no Prometheus duration such as 5m or 1h30m", name, key, d)
			}
		}
	}
	a := s.Alerts
	if a.QueueAgeSeconds < 1 || a.ScoreRegressionMinScores < 1 || a.ScoreRegressionPoints <= 0 || a.ScoreRegressionPoints > 100 {
		return fmt.Errorf("obsgen: alerts %+v: queueAgeSeconds and scoreRegressionMinScores must be at least 1, scoreRegressionPoints in (0, 100]", a)
	}
	return s.Cost.validate()
}

// currencyCode is the form the chart's schema accepts for cost.currency: an
// ISO 4217 code or nothing.
var currencyCode = regexp.MustCompile(`^([A-Z]{3})?$`)

// validate refuses a negative price and a currency that is no ISO 4217 code.
func (c CostSettings) validate() error {
	if c.PerJobSecond < 0 || c.PerJob < 0 {
		return fmt.Errorf("obsgen: cost prices %g, %g must not be negative", c.PerJobSecond, c.PerJob)
	}
	if !currencyCode.MatchString(c.Currency) {
		return fmt.Errorf("obsgen: cost.currency=%q is no ISO 4217 code such as EUR", c.Currency)
	}
	return nil
}

// promDuration is the duration form the chart's schema accepts, which
// Prometheus parses.
var promDuration = regexp.MustCompile(`^([0-9]+(ms|s|m|h|d|w|y))+$`)

// LatencyBounds are the values slo.scoreLatencySeconds may take: the bucket
// bounds of the Score request duration histogram, as its le label writes
// them.
func LatencyBounds() []string {
	var out []string
	for _, b := range m.RequestSecondsBuckets {
		out = append(out, bucketBound(b))
	}
	return out
}
