// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"strings"

	"github.com/golusoris/golusoris/config"
	gololog "github.com/golusoris/golusoris/log"
	"github.com/spf13/cobra"
	"go.uber.org/fx"

	"github.com/VMAFx/vmafx/internal/app/bootstrap"
)

// deps is the set of golusoris framework values injected into a vmafx-tune
// subcommand. Domain commands receive a *slog.Logger for structured
// diagnostics and a *config.Config for VMAFX_-prefixed runtime configuration.
type deps struct {
	Log *slog.Logger
	Cfg *config.Config
}

// configOptions overrides the golusoris config defaults so vmafx-tune reads the
// VMAFX_ environment prefix (ADR-1119). golusoris splits every underscore in an
// env var into the config delimiter, so VMAFX_LOG_LEVEL maps to the koanf path
// log.level — which the golusoris log module reads natively (no env bridge).
func configOptions() config.Options {
	return config.Options{
		EnvPrefix: "VMAFX_",
		Delimiter: ".",
		// File-watch is pointless for a short-lived one-shot CLI; disabling it
		// avoids spawning a watcher goroutine + SIGHUP handler per invocation.
		Watch: false,
	}
}

// levelledLogger decorates the golusoris-provided *slog.Logger so it honors the
// VMAFX_-prefixed log.level / log.format config keys.
//
// Why this is needed: golusoris.Core composes config + log as nested fx.Modules.
// A root-scope fx.Replace(config.Options{...}) (the override bootstrap.Base
// documents) is seen by root-scope consumers — including our domain code — but
// in golusoris v0.4.0 it does NOT penetrate the log submodule's own
// config.Options dependency, so the auto-built logger silently falls back to the
// default APP_ prefix and ends up at LevelInfo. Decorating *slog.Logger at root
// scope, where the config singleton already carries the VMAFX_-prefixed values,
// rebuilds the logger at the configured level. Drop this once golusoris makes
// the root override penetrate submodules (track upstream alongside #234).
func levelledLogger(cfg *config.Config, _ *slog.Logger) *slog.Logger {
	level, _ := gololog.LevelFromString(cfg.String("log.level"))
	return gololog.New(gololog.Options{
		Level:  level,
		Format: gololog.Format(strings.ToLower(cfg.String("log.format"))),
	})
}

// withGolusoris wraps a domain function so it runs inside a golusoris fx graph
// with config + slog injected, then adapts it to a clikit/cobra RunE handler.
//
// clikit.WithFx is designed for long-running services: its generated RunE calls
// app.Run(), which blocks until a signal and never surfaces an fx.Invoke error
// as the command's exit code. A one-shot tuning subcommand instead needs to (a)
// receive the injected dependencies, (b) run to completion, and (c) propagate a
// non-zero exit code on failure. We therefore build the fx app ourselves via
// bootstrap.Base, populate the dependencies, run the function, and return its
// error through clikit.WithRunE so cobra sets the process exit status.
func withGolusoris(run func(ctx context.Context, d deps, args []string) error) func(*cobra.Command, []string) error {
	return func(cmd *cobra.Command, args []string) error {
		ctx := cmd.Context()
		if ctx == nil {
			ctx = context.Background()
		}

		var d deps
		app := fx.New(
			bootstrap.Base,
			fx.Replace(configOptions()),
			// Rebuild the logger at the VMAFX_-configured level/format (see
			// levelledLogger for why the root config override does not otherwise
			// reach the log submodule in golusoris v0.4.0).
			fx.Decorate(levelledLogger),
			// Silence fx's own provide/invoke/lifecycle event stream. A one-shot
			// CLI must not flood stdout/stderr with dependency-graph chatter on
			// every invocation; the injected *slog.Logger still carries domain
			// diagnostics. (bootstrap.FxLogger(), which routes fx events onto the
			// app logger, is for long-running services, not a CLI.)
			fx.NopLogger,
			fx.Populate(&d.Log, &d.Cfg),
		)
		if err := app.Err(); err != nil {
			return fmt.Errorf("vmafx-tune: build dependency graph: %w", err)
		}

		startCtx, cancelStart := context.WithTimeout(ctx, app.StartTimeout())
		defer cancelStart()
		if err := app.Start(startCtx); err != nil {
			return fmt.Errorf("vmafx-tune: start dependency graph: %w", err)
		}

		runErr := run(ctx, d, args)

		stopCtx, cancelStop := context.WithTimeout(context.WithoutCancel(ctx), app.StopTimeout())
		defer cancelStop()
		stopErr := app.Stop(stopCtx)
		if stopErr != nil {
			stopErr = fmt.Errorf("vmafx-tune: stop dependency graph: %w", stopErr)
		}

		// Surface both the domain error and any shutdown error; errors.Join
		// drops nils, so the common (clean) path returns just runErr or nil
		// (AGENTS.md #7: errors.Join for multi-step cleanup).
		return errors.Join(runErr, stopErr)
	}
}
