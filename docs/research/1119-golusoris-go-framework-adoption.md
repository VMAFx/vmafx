<!-- markdownlint-disable MD013 -->
# Research digest 1119 — Adopting the golusoris fx framework across vmafx Go

**Question.** Can vmafx's six Go binaries and twelve `pkg/` libraries be
migrated onto [`github.com/golusoris/golusoris`](https://github.com/golusoris/golusoris)
(a `go.uber.org/fx` module framework) fully, and in what order, given the
maintainer's "adopt it fully; file issues for any missing capability" directive?

## Method

Read-only comparison of the golusoris source tree (at `v0.3.1`) against every
vmafx `cmd/*/main.go` + `pkg/*`. For each binary, mapped its hand-rolled
composition (logger, OTel, lifecycle, HTTP/gRPC server, config) onto the
equivalent golusoris module, and recorded what is **replaced** vs **kept**
(domain code). Diffed the two `go.mod` files to assess merge risk.

## Findings

1. **Dependency alignment is exact.** Both repos pin byte-identical versions of
   every shared dependency: `grpc v1.81.1`, `otel v1.44.0`,
   `controller-runtime v0.24.1`, `client-go v0.36.2`, `cobra v1.10.2`, MCP SDK
   `v1.6.1`, `go 1.26.4`. `go get golusoris@v0.3.1` + `go mod tidy` widened the
   closure (koanf, chi, fx/dig, river transitives) but `go build ./...` and all
   test binaries still compile clean — **no version-skew breakage**. This was
   the dominant a-priori risk and it is retired.

2. **Module coverage is near-complete.** golusoris provides every server
   primitive vmafx needs: `golusoris.Core` (config+log+clock+id+validate+crypto),
   `otel.Module`, `golusoris.HTTP` (chi + graceful `*http.Server`),
   `grpc.Module` (OTel/logging/recovery interceptors baked in), `k8s/health`
   probes, `k8s/operator` (a full controller-runtime manager fx module — not
   just health), `clikit` (cobra+fx), `leader`. The operator module is the
   single best fit: `vmafx-operator` reduces to `operator.Module +
   ProvideScheme(AddToScheme) + fx.Invoke(SetupWithManager)`.

3. **Three real gaps, filed upstream** (per the directive, not worked around):
   - **golusoris#225** — `grpc.Module` hard-codes its `ServerOption`s
     (`grpc/grpc.go:128-184`); no fx-injectable interceptor hook. **Blocks the
     controller**, which must chain a JWKS auth interceptor. Also requests a
     bounded `Stop()` fallback on shutdown (today `OnStop` only `GracefulStop()`s).
   - **golusoris#226** — no `version`/`buildinfo` module; every vmafx binary
     stamps `buildVersion` via ldflags. Interim: vmafx ships `version.Info` +
     `version.Get()` locally.
   - **golusoris#227** — `k8s/operator` does not call `ctrl.SetLogger` (so
     controller-runtime logs bypass slog) and `operator.Options` has no
     webhook-server port/cert field. Both are app-shimmable in the interim.

4. **Two deliberate non-adoptions** (design choices, not gaps): keep the `VMAFX_`
   config env-prefix via `fx.Replace(config.Options{EnvPrefix:"VMAFX_"})` rather
   than migrate the whole deployment surface to golusoris's default `APP_`; and
   keep the controller's embedded `modernc.org/sqlite` job queue rather than
   adopt `golusoris.Jobs` (river/Postgres), which would force a Postgres
   dependency onto a single-binary design. golusoris#99 itself frames
   river/Postgres as a deliberate framework strength, confirming this is not a
   gap to backfill.

## Recommendation

Phase 0 foundation (`go get` + `internal/app/bootstrap` + this digest + ADR-1119),
then services-first: `vmafx-server` (proves Core+HTTP+gRPC+health with no
blockers) → `vmafx-controller` (after #225 lands) → `vmafx-node` →
`vmafx-operator` (parallelisable — shares no scoring code). Then CLI tools
(`vmafx-mcp`, `vmafx-tune` via clikit) and the `pkg/` sweep. Keep `pkg/*`
framework-agnostic: resolve config in each binary's providers and pass plain
typed args down, so the libraries stay unit-testable without fx.

The chief residual correctness risk is cgo-scorer lifetime under fx `OnStop`
ordering — the gRPC server must drain in-flight `Score` calls before the
`libvmaf.Scorer` `Close()`s its C resources; enforced by the provider
dependency edge (gRPC depends on Scorer ⇒ fx stops gRPC first).

## References

- ADR: [ADR-1119](../adr/1119-golusoris-go-framework-adoption.md).
- Plan: `.workingdir2/rc/golusoris/PLAN.md`.
- golusoris epic [#99](https://github.com/golusoris/golusoris/issues/99); gaps [#225](https://github.com/golusoris/golusoris/issues/225) / [#226](https://github.com/golusoris/golusoris/issues/226) / [#227](https://github.com/golusoris/golusoris/issues/227).
