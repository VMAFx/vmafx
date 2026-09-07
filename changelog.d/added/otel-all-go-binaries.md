- OpenTelemetry on every Go binary through the shared `internal/app/bootstrap`
  stanza (ADR-0782, ADR-1119; epic #1241). `bootstrap.Base` now stamps
  `service.version` from `pkg/version` and honours `OTEL_SERVICE_NAME` on
  `vmafx-server`, `vmafx-controller`, `vmafx-node`, `vmafx-operator`,
  `vmafx-mcp` and `vmafx-tune`; HTTP routes on the server, controller and the
  MCP HTTP transport get `otelhttp` server spans (`bootstrap.HTTPTracing` /
  `TraceHTTPHandler`, probes and `/metrics` filtered); the operator's
  `GetJob` poll dials through golusoris's `ConnFactory` so it joins the
  controller's trace; new job spans `vmafx.mcp.tool` (per MCP tool call) and
  `vmafx.tune.command` (per `vmafx-tune` subcommand), plus
  `vmafx.onnx.inference` around the `vmafx-ort-runner` subprocess in `pkg/ai`.
  `vmafx-ort-runner` itself stays OTel-free (ADR-1134). Export is off until
  `OTEL_EXPORTER_OTLP_ENDPOINT` (or `VMAFX_OTEL_ENDPOINT`) is set; operator
  guide rewritten at `docs/development/observability.md`.
