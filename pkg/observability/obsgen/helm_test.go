// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"testing"

	"github.com/santhosh-tekuri/jsonschema/v6"
	"go.yaml.in/yaml/v3"
)

func readRepoFile(t *testing.T, path string) []byte {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(repoRoot, filepath.FromSlash(path)))
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

// TestChartValuesCarryTheDefaults: the generated block of the chart's
// values.yaml sits under monitoring: and reads back as DefaultSettings.
func TestChartValuesCarryTheDefaults(t *testing.T) {
	t.Parallel()
	got, err := Settings{}.ApplyValues(readRepoFile(t, HelmValuesFile))
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got, DefaultSettings()) {
		t.Errorf("values.yaml monitoring settings = %+v, want %+v", got, DefaultSettings())
	}
}

// TestRegionMerge replaces only the block between the markers, indented as
// the BEGIN marker, and refuses a file without them.
func TestRegionMerge(t *testing.T) {
	t.Parallel()
	f := File{Path: "v.yaml", Region: "r", Content: []byte("a: 1\nb:\n  c: 2\n")}
	old := "top:\n  keep: x\n  # BEGIN r\n  stale: y\n  # END r\nafter: z\n"
	got, err := f.Merge([]byte(old))
	if err != nil {
		t.Fatal(err)
	}
	want := "top:\n  keep: x\n  # BEGIN r\n  a: 1\n  b:\n    c: 2\n  # END r\nafter: z\n"
	if string(got) != want {
		t.Errorf("Merge =\n%s\nwant\n%s", got, want)
	}
	if again, _ := f.Merge(got); !bytes.Equal(again, got) {
		t.Error("Merge is not idempotent")
	}
	if _, err := f.Merge([]byte("top: {}\n")); err == nil {
		t.Error("Merge accepted a file without the region markers")
	}
}

// TestApplyValues overlays only the keys a document sets and refuses an
// unknown key and an invalid value.
func TestApplyValues(t *testing.T) {
	t.Parallel()
	got, err := DefaultSettings().ApplyValues([]byte("image: {tag: x}\nmonitoring:\n  enabled: true\n  slo: {jobSuccess: 0.995}\n  burnRates: {slow: {for: 1h}}\n"))
	if err != nil {
		t.Fatal(err)
	}
	want := DefaultSettings()
	want.SLO.JobSuccess = 0.995
	want.BurnRates.Slow.For = "1h"
	if !reflect.DeepEqual(got, want) {
		t.Errorf("ApplyValues = %+v, want %+v", got, want)
	}
	for _, bad := range []string{
		"monitoring: {slo: {jobsuccess: 0.9}}",
		"monitoring: {alerts: {queueAgeSeconds: 1.5}}",
		"monitoring: {slo: {jobSuccess: 1}}",
		"monitoring: [",
	} {
		if _, err := DefaultSettings().ApplyValues([]byte(bad)); err == nil {
			t.Errorf("ApplyValues(%q) accepted", bad)
		}
	}
}

// chartSchema compiles the chart's values.schema.json.
func chartSchema(t *testing.T) *jsonschema.Schema {
	t.Helper()
	doc, err := jsonschema.UnmarshalJSON(bytes.NewReader(readRepoFile(t, HelmChartDir+"/values.schema.json")))
	if err != nil {
		t.Fatal(err)
	}
	c := jsonschema.NewCompiler()
	if err := c.AddResource("values.schema.json", doc); err != nil {
		t.Fatal(err)
	}
	s, err := c.Compile("values.schema.json")
	if err != nil {
		t.Fatal(err)
	}
	return s
}

// chartValuesWith returns the chart's values.yaml with override's
// monitoring keys merged in, as the JSON instance the schema validates.
func chartValuesWith(t *testing.T, override string) any {
	t.Helper()
	var values, over map[string]any
	if err := yaml.Unmarshal(readRepoFile(t, HelmValuesFile), &values); err != nil {
		t.Fatal(err)
	}
	if err := yaml.Unmarshal([]byte(override), &over); err != nil {
		t.Fatal(err)
	}
	mergeValues(values, over)
	raw, err := json.Marshal(values)
	if err != nil {
		t.Fatal(err)
	}
	inst, err := jsonschema.UnmarshalJSON(bytes.NewReader(raw))
	if err != nil {
		t.Fatal(err)
	}
	return inst
}

// mergeValues merges src into dst as Helm merges values: maps key by key,
// anything else replaced. A work list of map pairs, no recursion (HISS-01).
func mergeValues(dst, src map[string]any) {
	type pair struct{ dst, src map[string]any }
	todo := []pair{{dst, src}}
	for len(todo) > 0 {
		p := todo[len(todo)-1]
		todo = todo[:len(todo)-1]
		for k, v := range p.src {
			sub, ok := v.(map[string]any)
			if d, isMap := p.dst[k].(map[string]any); ok && isMap {
				todo = append(todo, pair{d, sub})
				continue
			}
			p.dst[k] = v
		}
	}
}

// TestValidateAgreesWithTheChartSchema: every case is accepted by both the
// chart's schema and Settings.Validate (through ApplyValues), or refused by
// both, so the Compose example and the chart take the same settings.
func TestValidateAgreesWithTheChartSchema(t *testing.T) {
	t.Parallel()
	schema := chartSchema(t)
	cases := map[string]bool{
		"monitoring: {}": true,
		"monitoring: {slo: {jobSuccess: 0.5, scoreLatencySeconds: '0.25'}}":     true,
		"monitoring: {slo: {jobSuccess: 1}}":                                    false,
		"monitoring: {slo: {scoreSuccess: 0}}":                                  false,
		"monitoring: {slo: {scoreLatencySeconds: '31'}}":                        false,
		"monitoring: {slo: {scoreLatencySeconds: 30}}":                          false,
		"monitoring: {slo: {objective: 0.9}}":                                   false,
		"monitoring: {burnRates: {fast: {factor: 0}}}":                          false,
		"monitoring: {burnRates: {fast: {longWindow: 1h30m}}}":                  true,
		"monitoring: {burnRates: {slow: {shortWindow: 1x}}}":                    false,
		"monitoring: {burnRates: {slow: {for: ''}}}":                            false,
		"monitoring: {alerts: {queueAgeSeconds: 0}}":                            false,
		"monitoring: {alerts: {queueAgeSeconds: 1.5}}":                          false,
		"monitoring: {alerts: {scoreRegressionPoints: 100}}":                    true,
		"monitoring: {alerts: {scoreRegressionPoints: 101}}":                    false,
		"monitoring: {alerts: {scoreRegressionMinScores: 1}}":                   true,
		"monitoring: {cost: {perJobSecond: 0.002, perJob: 0.1, currency: EUR}}": true,
		"monitoring: {cost: {perJobSecond: -1}}":                                false,
		"monitoring: {cost: {currency: euro}}":                                  false,
		"monitoring: {cost: {perJob: '1'}}":                                     false,
	}
	for doc, valid := range cases {
		schemaErr := schema.Validate(chartValuesWith(t, doc))
		_, goErr := DefaultSettings().ApplyValues([]byte(doc))
		if (schemaErr == nil) != valid || (goErr == nil) != valid {
			t.Errorf("%s: want valid=%v; schema: %v; ApplyValues: %v", doc, valid, schemaErr, goErr)
		}
	}
}

// TestChartSchemaMatchesSettings: the schema's latency bounds are the
// histogram's buckets and its duration pattern is promDuration's.
func TestChartSchemaMatchesSettings(t *testing.T) {
	t.Parallel()
	var schema struct {
		Properties struct {
			Monitoring struct {
				Properties struct {
					SLO struct {
						Properties struct {
							Latency struct {
								Enum []string `json:"enum"`
							} `json:"scoreLatencySeconds"`
						} `json:"properties"`
					} `json:"slo"`
				} `json:"properties"`
			} `json:"monitoring"`
		} `json:"properties"`
		Defs struct {
			Duration struct {
				Pattern string `json:"pattern"`
			} `json:"promDuration"`
		} `json:"$defs"`
	}
	if err := json.Unmarshal(readRepoFile(t, HelmChartDir+"/values.schema.json"), &schema); err != nil {
		t.Fatal(err)
	}
	if got := schema.Properties.Monitoring.Properties.SLO.Properties.Latency.Enum; !slices.Equal(got, LatencyBounds()) {
		t.Errorf("schema scoreLatencySeconds enum %v, want the buckets %v", got, LatencyBounds())
	}
	if got := schema.Defs.Duration.Pattern; got != promDuration.String() {
		t.Errorf("schema promDuration pattern %q, want %q", got, promDuration.String())
	}
}

// TestHelmTemplateUsesEverySetting: each settings key the chart's values
// carry appears in the PrometheusRule template, so no value is ignored.
func TestHelmTemplateUsesEverySetting(t *testing.T) {
	t.Parallel()
	tmpl := string(readRepoFile(t, HelmRuleTemplate))
	for _, key := range []string{
		"slo.jobSuccess", "slo.scoreSuccess", "slo.scoreLatency ", "slo.scoreLatencySeconds",
		"burnRates.fast.longWindow", "burnRates.fast.shortWindow", "burnRates.fast.factor", "burnRates.fast.for",
		"burnRates.slow.longWindow", "burnRates.slow.shortWindow", "burnRates.slow.factor", "burnRates.slow.for",
		"alerts.queueAgeSeconds", "alerts.scoreRegressionPoints", "alerts.scoreRegressionMinScores",
		"cost.perJobSecond", "cost.perJob", "cost.currency",
	} {
		if !strings.Contains(tmpl, helmValues+"."+key) {
			t.Errorf("%s does not read %s.%s", HelmRuleTemplate, helmValues, key)
		}
	}
}
