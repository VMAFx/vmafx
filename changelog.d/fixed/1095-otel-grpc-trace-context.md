- **fix(observability): OTel trace context not propagated across gRPC server/client boundaries (ADR-1095).**
  `vmafx-server` gRPC server was missing `otelgrpc.NewServerHandler()`, so incoming `traceparent` headers
  were silently discarded and every server span appeared as an unrooted trace root.
  `pkg/score.Dial` was missing `otelgrpc.NewClientHandler()`, so outgoing RPCs carried no `traceparent`
  header and could not be linked to the controller's spans.
  `ObserveScoreLatency` passed `context.Background()` to the histogram `Record` call, discarding baggage
  and preventing OTel exemplar attachment.
  All three gaps are fixed; distributed traces across controller → server → node now appear as a single
  linked waterfall in Jaeger / Grafana Tempo.
