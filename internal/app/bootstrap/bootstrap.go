// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package bootstrap centralises the golusoris fx composition shared by every
// vmafx binary (ADR-1119). Each binary's main() starts from [Base] and adds
// its own server modules (golusoris.HTTP / grpc.Module / k8s/operator) plus
// its domain providers. Wiring the common stanza here keeps the composition
// root identical across cmd/vmafx-{server,controller,node,operator,mcp,tune}.
package bootstrap

import (
	"log/slog"

	"github.com/golusoris/golusoris"
	"github.com/golusoris/golusoris/otel"
	"go.uber.org/fx"
	"go.uber.org/fx/fxevent"

	"github.com/VMAFx/vmafx/pkg/version"
)

// Base is the module set every vmafx service shares:
//
//   - golusoris.Core — config (koanf; binaries override the env prefix to
//     "VMAFX_" via fx.Replace(config.Options{...}) per ADR-1119), structured
//     slog logging, clock, id, validate, crypto.
//   - otel.Module — OpenTelemetry tracer/meter/logger over OTLP; a silent
//     no-op when no exporter endpoint is configured.
//   - the build version, supplied into the graph until golusoris ships a
//     version module (golusoris#226).
//
// It deliberately does NOT include a server module: a binary picks
// golusoris.HTTP, grpc.Module, and/or k8s/operator as appropriate.
var Base = fx.Options(
	golusoris.Core,
	otel.Module,
	fx.Supply(version.Get()),
)

// FxLogger routes fx's own lifecycle events (provide/invoke/start/stop) onto
// the golusoris *slog.Logger so dependency-graph and lifecycle events share
// the application log stream and its OTel correlation, instead of fx's default
// stderr printer. Add it to a binary's fx.New(...) alongside [Base].
func FxLogger() fx.Option {
	return fx.WithLogger(func(l *slog.Logger) fxevent.Logger {
		return &fxevent.SlogLogger{Logger: l}
	})
}
