- `pkg/observability`: add eight branch-coverage tests raising statement
  coverage from 89.1 % to 91.3 %. New tests cover: `WaitForShutdown` SIGTERM
  signal-delivery path; `InitOTel` with valid `OTEL_TRACES_SAMPLER_ARG`
  values in [0.0, 1.0] (0.0, 0.5, 1.0 boundary checks); half-nil
  `SetControllerSources` cases (q=non-nil+r=nil, q=nil+r=non-nil); Prometheus
  registry isolation between two independent `Metrics` instances (ADR-1014);
  OTel attribute key string values locked to ADR-0782 schema.
