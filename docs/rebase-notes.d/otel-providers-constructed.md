## OpenTelemetry: Base builds the providers (2026-10-07)

`fix/otel-providers-constructed`, fork-only. `internal/app/bootstrap.Base` ends with
`fx.Invoke(func(*otel.Providers) {})`; without it fx never builds golusoris's OTel providers and no binary exports anything.
Keep it on a rebase or a golusoris bump, unless golusoris's `otel.Module` invokes them itself.
`TestBase_ConstructsProvidersNobodyRequests` guards it.
