- **Traces, metrics and logs leave the services when an OTLP endpoint is
  set.** `vmafx-server`, `vmafx-controller`, `vmafx-node`, `vmafx-operator`
  and `vmafx-mcp` never built their OpenTelemetry exporters, so nothing was
  exported whatever the endpoint; they now build them at start and log
  `otel: configured` with `active=true`. The documented
  `OTEL_EXPORTER_OTLP_ENDPOINT=host:4317` form sent everything to
  `localhost:4317`: the standard variable takes a URL
  (`http://otel-collector:4317`), `VMAFX_OTEL_ENDPOINT` takes `host:port`. See
  [OpenTelemetry](docs/observability/otel.md#environment-variables).
