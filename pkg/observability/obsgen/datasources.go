// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package obsgen

// DatasourcesFile is the generated Grafana data source provisioning file. It
// links the three signals: a latency exemplar opens its trace in Tempo
// (exemplarTraceIdDestinations), a span opens the logs of its trace in Loki
// (tracesToLogsV2), and a log line's trace_id opens the trace
// (derivedFields). The URLs are the service names of the Compose example;
// another deployment changes the URLs and keeps the links.
const DatasourcesFile = "deploy/grafana/provisioning/datasources/vmafx.yaml"

// Data source uids the links refer to.
const (
	PrometheusUID = "vmafx-prometheus"
	TempoUID      = "vmafx-tempo"
	LokiUID       = "vmafx-loki"
)

type datasourceFile struct {
	APIVersion  int          `yaml:"apiVersion"`
	Datasources []datasource `yaml:"datasources"`
}

type datasource struct {
	Name      string         `yaml:"name"`
	UID       string         `yaml:"uid"`
	Type      string         `yaml:"type"`
	Access    string         `yaml:"access"`
	URL       string         `yaml:"url"`
	IsDefault bool           `yaml:"isDefault"`
	JSONData  map[string]any `yaml:"jsonData"`
}

// datasourcesYAML renders the provisioning file. "$$" is Grafana's escape for
// a literal "$" in provisioning files.
func datasourcesYAML() ([]byte, error) {
	return marshalYAML(datasourceFile{APIVersion: 1, Datasources: []datasource{
		{Name: "Prometheus", UID: PrometheusUID, Type: "prometheus", Access: "proxy", URL: "http://prometheus:9090", IsDefault: true,
			JSONData: map[string]any{
				"exemplarTraceIdDestinations": []map[string]string{{"name": "trace_id", "datasourceUid": TempoUID}},
			}},
		{Name: "Tempo", UID: TempoUID, Type: "tempo", Access: "proxy", URL: "http://tempo:3200",
			JSONData: map[string]any{
				"tracesToLogsV2": map[string]any{
					"datasourceUid": LokiUID, "spanStartTimeShift": "-5m", "spanEndTimeShift": "5m",
					"customQuery": true,
					"query":       `{service_name="$${__span.tags["service.name"]}"} | trace_id="$${__trace.traceId}"`,
				},
			}},
		{Name: "Loki", UID: LokiUID, Type: "loki", Access: "proxy", URL: "http://loki:3100",
			JSONData: map[string]any{
				"derivedFields": []map[string]string{{
					"name": "trace_id", "matcherType": "label", "matcherRegex": "trace_id",
					"datasourceUid": TempoUID, "url": "$${__value.raw}",
				}},
			}},
	}})
}
