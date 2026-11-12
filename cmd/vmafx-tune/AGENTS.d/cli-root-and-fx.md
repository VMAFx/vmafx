---
paths:
  - cmd/vmafx-tune/main*.go
  - cmd/vmafx-tune/cmd/root*.go
  - cmd/vmafx-tune/cmd/golusoris.go
  - cmd/vmafx-tune/cmd/otel_test.go
  - cmd/vmafx-tune/cmd/required_flags*.go
  - cmd/vmafx-tune/cmd/exitcode*.go
invariant: Install as vmafx-tune-go; one-shot withGolusoris + fx.NopLogger; command span; usage errors exit 2.
---
<!-- markdownlint-disable MD013 MD029 -->
# CLI binary name, `withGolusoris` fx adapter, OTel span and exit codes

## Rebase-sensitive invariants

6. **Binary name** (`cmd/vmafx-tune/main.go`): binary installs as
   `vmafx-tune-go`, not `vmaf-tune`, during Stage 1 to avoid collisions with
   Python binary. Stage 3 (swap) will rename. Never install it as `vmaf-tune`
   in PR that does not also remove Python entry point.

10. **clikit root + one-shot fx adapter** (`cmd/vmafx-tune/cmd/root.go`,
    `golusoris.go`): CLI root built with `clikit.New`, every
    subcommand with `clikit.Command` (ADR-1119 Phase-1). One-shot subcommands
    (compare, ladder, report, not-yet-ported stubs) wire handler
    via `clikit.WithRunE(withGolusoris(fn))`. `withGolusoris` boots `fx`
    graph from `bootstrap.Base`, `fx.Populate`s `*slog.Logger` + `*config.Config`
    into handler, runs `fn`, then `app.Stop`s — returns `fn`'s error so
    cobra sets process exit code. **Do not** swap these to
    `clikit.WithFx(bootstrap.Core, fx.Invoke(fn))`: clikit's `WithFx` calls
    `app.Run()` (blocks until signal) and discards `fx.Invoke` error, so
    one-shot command would hang, lose exit code. New subcommands follow
    same `withGolusoris` shape; keep `run*` signature
    `func(ctx context.Context, d deps, ...) error` so logger/config injection
    flows through.

11. **`fx.NopLogger`, not `bootstrap.FxLogger()`** (`cmd/vmafx-tune/cmd/golusoris.go`):
    `withGolusoris` attaches `fx.NopLogger` so fx's own provide/invoke/lifecycle
    events never reach console. `bootstrap.FxLogger()` (routes fx
    events onto app `*slog.Logger`) = for long-running services; on
    one-shot CLI floods `stderr` with dependency-graph chatter on every
    invocation. Domain diagnostics still go through injected `*slog.Logger`.

12. **Log level comes natively from VMAFX_-prefixed config** (golusoris
    v0.5.0, #234): `golusoris.log` submodule reads `log.level` / `log.format`
    from shared config singleton, so root-scope
    `fx.Replace(config.Options{EnvPrefix:"VMAFX_"})` penetrates, and
    auto-built `*slog.Logger` honors `VMAFX_LOG_LEVEL` / `VMAFX_LOG_FORMAT` with
    **no decorator**. (Earlier `levelledLogger` v0.4.0 workaround removed
    when pin moved to v0.5.0.) `TestGolusorisInjection_ConfigDrivesLogLevel`
    guards behavior — builds graph without any decorator, matching
    `withGolusoris()` exactly.

29. **One `vmafx.tune.command` span per invocation, from `withGolusoris`**
    (`cmd/vmafx-tune/cmd/golusoris.go`, ADR-0782 / ADR-1119): shared
    one-shot fx adapter = CLI's only OTel seam. Builds
    `bootstrap.Base` (golusoris `otel.Module`, no-op without OTLP endpoint,
    flushed by `app.Stop`), starts span with `AttrTuneCommand` =
    `cmd.CommandPath()`, ends it **before** `app.Stop` so OnStop flush
    exports it. Subcommands must not start own root span or call
    `InitOTel`; `deps.OTel` exists so `otel_test.go` can prove wiring.
    `TestWithGolusoris_CommandSpanWrapsRun` locks span, its attribute and
    its error status.

30. **Usage failures exit 2 on every subcommand** (`root.go`,
    `required_flags.go`, `exitcode.go`): `newRoot` installs
    `useUsageExitCode` on root (cobra inherits `FlagErrorFunc`), so unknown
    or unparseable flags exit 2; `markCommandFlagsRequired` runs cobra's
    `ValidateRequiredFlags` from `PreRunE`, ahead of cobra's own check
    (exit 1), and tags it with `asUsageError`. flag required only in some
    modes (`auto --src`, required unless `--smoke`) is checked in run
    function with `asUsageError`, never with `MarkFlagRequired`.
    `TestEveryCommandRejectsUnknownFlagWithUsageStatus` and
    `TestMissingRequiredFlagExitsWithUsageStatus` cover new subcommands
    automatically for first and by table for second: add row
    when command gains required flag.
