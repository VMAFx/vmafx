<!-- markdownlint-disable MD013 -->
# ADR-1119: Adopt the golusoris fx framework across all vmafx Go binaries

- **Status**: Accepted
- **Date**: 2026-06-14
- **Deciders**: Lusoris
- **Tags**: go, framework, fx, golusoris, server, controller, node, operator, mcp, vmaf-tune, rc-blocking, fork-local

## Context

vmafx ships a fleet of six Go binaries — `cmd/vmafx-{server,controller,node,operator,mcp,tune}`
— plus twelve `pkg/` libraries. Each binary hand-rolls its own composition
root: bespoke `slog` setup, a private `observability.InitOTel` + shutdown
dance, manual `signal.NotifyContext` + `errgroup` lifecycle, hand-built
`*http.Server` / `grpc.NewServer(...)`, and ad-hoc `os.Getenv` config. The same
~150 lines of boilerplate are copy-pasted across the fleet and drift
independently.

The maintainer's RC condition is that **all** vmafx Go code adopt the
sibling-org framework [`github.com/golusoris/golusoris`](https://github.com/golusoris/golusoris)
— a thin, composable set of `go.uber.org/fx` modules (config via koanf, slog
logging, OTel, chi HTTP, a gRPC server with OTel/logging/recovery interceptors
baked in, a controller-runtime operator module, clikit for CLIs, health
probes). The maintainer's directive is explicit: adopt the framework **fully**
(every binary becomes an `fx.New(...).Run()` composition over golusoris
modules), and where golusoris is **missing a capability vmafx needs, file an
issue on the golusoris repo** for the maintainer to integrate upstream — rather
than designing a vmafx-local workaround.

A read-only design pass (full per-binary plan in
`.workingdir2/rc/golusoris/PLAN.md`) established that the migration is
**low-risk on dependencies**: both repos already pin byte-identical versions of
every shared dependency (`grpc v1.81.1`, `otel v1.44.0`,
`controller-runtime v0.24.1`, `client-go v0.36.2`, `cobra v1.10.2`,
MCP SDK `v1.6.1`, `go 1.26.4`), so the `go.mod` merge carries no version-skew
risk. The same pass surfaced three real framework gaps (filed as golusoris
issues, see References) and one non-code decision (the config env-prefix),
captured below.

## Decision

1. **Full fx adoption, pinned `golusoris v0.4.0`.** Every vmafx Go binary is
   restructured as `fx.New(...)` over golusoris modules. A shared, vmafx-local
   `internal/app/bootstrap` package provides the common stanza
   (`bootstrap.Base = golusoris.Core + otel.Module + fx.Supply(version.Info)`
   and an `FxLogger()` helper routing fx events onto the golusoris `*slog.Logger`)
   so the boilerplate is wired once and imported by all six binaries.

2. **Keep the `VMAFX_` config env-prefix** (do not migrate the deployment
   surface to golusoris's default `APP_`). golusoris's `config.Module` is
   parameterised, so each binary overrides the prefix with
   `fx.Replace(config.Options{EnvPrefix: "VMAFX_", Delimiter: "."})` ahead of
   `golusoris.Core`. This preserves the existing Helm/k8s/`dev/Containerfile`/docs
   env contract; a prefix migration would be a large, breaking ops churn for no
   functional gain.

3. **RC-blocking, phased, services first.** Phase 0 is this foundation
   (`go get`, `bootstrap`, this ADR). Phase 1 migrates the production services
   in order `vmafx-server` → `vmafx-controller` → `vmafx-node` →
   `vmafx-operator`. Phase 2 migrates the CLI tools (`vmafx-mcp`, `vmafx-tune`)
   and finishes the `pkg/` sweep. `vmafx-server` goes first because it
   exercises the full common stanza (Core + otel + HTTP + gRPC + health) with
   **no** auth interceptor, controller-runtime, embedded queue, or stdio
   constraint — proving every reusable pattern the later binaries copy.

4. **Missing golusoris capabilities are filed upstream, not worked around.**
   The controller's gRPC migration is **gated on golusoris#225** (no
   fx-injectable interceptor hook today — vmafx needs to chain its JWKS auth
   interceptor); the controller is sequenced after that lands. `version`/`buildinfo`
   (golusoris#226) and the operator `SetLogger`/webhook-config polish
   (golusoris#227) are non-blocking — vmafx supplies its own `version.Info` and a
   one-line `ctrl.SetLogger` `fx.Invoke` in the interim, to be removed when the
   framework absorbs them.

5. **Domain packages stay framework-agnostic.** `pkg/*` (the cgo `libvmaf`
   scorer, encoder/probe subprocess wrappers, ladder/report math, rclone
   storage, ONNX `ai` registry) do **not** import `*config.Config`; values are
   resolved in each binary's `fx.Provide` provider and passed as plain typed
   args. This keeps the libraries unit-testable without fx. The controller's
   embedded `modernc.org/sqlite` job queue is **kept** as domain code — it is a
   deliberate single-binary design choice, not a gap to backfill with
   golusoris's Postgres/river `Jobs` module.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Full fx adoption, pinned v0.4.0, phased services-first (chosen)** | Eliminates ~150 LOC/binary of duplicated composition; one place to fix lifecycle/otel/health bugs; aligns vmafx with the sibling-org standard; gaps drive upstream improvements | Six binaries to migrate; controller blocked on one upstream fix; an env-prefix decision to document | Selected — matches the maintainer's explicit "full + file-issues-for-gaps" directive and the de-risked dependency alignment |
| Partial adoption (Core + log + otel only, keep hand-rolled servers) | Smaller blast radius | Leaves the duplicated `*http.Server`/`grpc.NewServer` boilerplate the framework exists to remove; half-migration drifts | Rejected — does not satisfy "all golang code uses the framework" |
| Float on `@main` / `@latest` instead of a pin | Picks up gap fixes (#225/#226/#227) automatically | Non-reproducible builds; a moving target across six in-flight PRs; supply-chain risk | Rejected — pin v0.3.1; bump deliberately when the gap fixes tag |
| Migrate env-prefix `VMAFX_` → golusoris default `APP_` | Uses framework defaults verbatim | Breaking change to every Helm chart, k8s manifest, Containerfile, and doc; large ops churn, zero functional gain | Rejected — override the prefix via `fx.Replace` and keep the contract |
| Adopt `golusoris.Jobs` (river/Postgres) for the controller queue | One less bespoke component | Forces a Postgres dependency onto a deliberately single-binary embedded-SQLite design; changes deployment topology | Rejected — keep the embedded queue as domain code (golusoris#99 frames river/Postgres as a deliberate framework strength, not a mandate) |

## Consequences

- **Positive**: a single composition idiom across the fleet; lifecycle, OTel,
  graceful shutdown, and health probes owned by the framework and fixed once;
  new binaries start from `bootstrap.Base`; framework gaps found by a real
  consumer flow back upstream (golusoris#225/#226/#227).
- **Negative**: a multi-PR migration that touches every binary's `main`; the
  controller is blocked until golusoris#225 ships an interceptor-injection hook
  (interim: a vmafx-local gRPC provider mirroring the framework chain plus the
  auth interceptor, deleted once #225 lands); `pkg/observability` is split (the
  OTel/logger/shutdown helpers are dropped in favour of framework modules; the
  Prometheus `Metrics` struct is kept and renamed `pkg/metrics`, since golusoris
  OTel is OTLP, not a Prometheus registry).
- **Neutral / follow-ups**: the `go.mod` closure widens (koanf, chi, river,
  rueidis, casbin enter the graph via the `golusoris` umbrella aliases) — import
  sub-packages directly where possible and re-run `govulncheck` + watch binary
  size after Phase 0. cgo-scorer lifetime under fx `OnStop` ordering (the gRPC
  server must drain before the scorer `Close()`s) is the key correctness risk,
  handled by the provider dependency edge.
- **Update (2026-06-15)**: the maintainer integrated and **closed all four**
  filed gaps and the pin was bumped `v0.3.1 → v0.4.0` (popup decision). The
  **v0.4.0 tag carries only #226 (version) + the `k8s/operator` module**; #225
  (gRPC `ServerOption` fx-group injection), #227 (operator `SetLogger` + webhook
  config) and #234 (logger reads `log.level` from the koanf tree) are merged to
  golusoris `main` but **not yet tagged**. So on v0.4.0 each service binary
  keeps a small interim shim — the `VMAFX_LOG_LEVEL/LOG_FORMAT → bare-env` bridge
  for #234 (a `levelledLogger` decorator in vmafx-tune), and the operator's
  `ctrl.SetLogger` + config-gated webhook wiring for #227 — and the
  **controller** (which needs #225) waits for the next golusoris tag. Cutting a
  `v0.4.1`/`v0.5.0` tag off `main` would drop every shim and unblock the
  controller in one step. Decision §4's interim-shim plan therefore still holds
  on the v0.4.0 tag and is retired tag-by-tag as the fixes land.

## References

- golusoris roadmap epic: [golusoris#99](https://github.com/golusoris/golusoris/issues/99) (VMAFx pattern extraction).
- Filed gaps: [golusoris#225](https://github.com/golusoris/golusoris/issues/225) (gRPC interceptor injection + hard-stop — **blocks the controller**), [golusoris#226](https://github.com/golusoris/golusoris/issues/226) (version/buildinfo module), [golusoris#227](https://github.com/golusoris/golusoris/issues/227) (operator SetLogger + webhook config).
- Per-binary migration plan: `.workingdir2/rc/golusoris/PLAN.md`.
- Research digest: [Research-1119](../research/1119-golusoris-go-framework-adoption.md).
- Source: `req` (user direction — all golang code must use the github.com/golusoris/golusoris framework; adopt it fully; where the framework lacks a needed capability, open an issue on the golusoris repo for the maintainer to integrate rather than designing a local workaround; RC-blocking, phased with the production services first, pinned initially at v0.3.1, then bumped to v0.4.0 once the maintainer integrated all four filed gaps — #225/#226/#227/#234 — and the k8s/operator module landed there).
