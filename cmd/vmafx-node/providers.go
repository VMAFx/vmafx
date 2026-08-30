// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/providers.go — fx domain providers for vmafx-node (ADR-1119).
//
// These constructors translate the golusoris config tree into the node's domain
// objects and register their lifecycle hooks. They replace the imperative
// startup sequence the pre-fx main() ran inline (ffmpeg discovery → encoder
// probe → scorer construction → server.Serve). The domain logic itself
// (probe/, executor.go, online_feedback.go, the scoring handler) is unchanged;
// only its wiring moves under fx.
//
// Lifecycle ordering (R-node), in fx construction order — OnStop runs in
// reverse:
//
//	provideEncoderInventory  OnStart: run probe (NON-FATAL)
//	provideScorer            OnStop:  scorer.Close()           (4th to stop)
//	provideFeedbackClient    OnStart: start drainer            (3rd to stop)
//	                         OnStop:  Close() + await drainer
//	grpc.Module              OnStart: bind + serve             (1st to stop)
//	                         OnStop:  GracefulStop (drain RPCs)
//
// The standalone fx.Invoke(func(_ *libvmaf.Scorer){}) and the gRPC registration
// invoke in main pin the relative order so the gRPC server drains in-flight
// Score / ScoreStream calls before the FeedbackClient drainer stops and before
// the scorer closes.

//go:build cgo

package main

import (
	"context"
	"log/slog"
	"os"

	"go.uber.org/fx"

	"github.com/golusoris/golusoris/config"

	"github.com/VMAFx/vmafx/cmd/vmafx-node/probe"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// provideEncoderInventory constructs a shared encoder Inventory and runs the
// startup probe in an OnStart hook. The probe enumerates `ffmpeg -encoders` and
// is bounded by probeTimeout so a hung ffmpeg binary cannot stall node startup.
//
// Probe failure is NON-FATAL: the returned Inventory starts empty and is
// populated in place on success, so a node whose ffmpeg is missing or wedged
// still serves (encoders that are unavailable fail at job-dispatch time with a
// clear error). The Inventory pointer is shared with the scoring handler; the
// probe completes in OnStart strictly before the gRPC listener binds (the
// Inventory is constructed before the *grpc.Server, so its OnStart hook is
// appended — and therefore runs — first), so there is no concurrent read of the
// slices it fills.
func provideEncoderInventory(lc fx.Lifecycle, cfg *config.Config, log *slog.Logger) *probe.Inventory {
	ffmpegBin := ffmpegBinary(cfg)
	inv := probe.EmptyInventory()
	lc.Append(fx.Hook{
		OnStart: func(ctx context.Context) error {
			probeCtx, cancel := context.WithTimeout(ctx, probeTimeout)
			defer cancel()
			result, err := probe.EncoderInventory(probeCtx, ffmpegBin)
			if err != nil {
				// NON-FATAL: degrade to an empty inventory and keep serving.
				log.Warn("encoder probe failed — node will serve with empty inventory",
					"error", err, "ffmpeg", ffmpegBin)
				return nil
			}
			*inv = *result
			log.Info("encoder inventory",
				"ffmpeg", ffmpegBin,
				"count", len(inv.Available),
				"available", inv.Available,
				"missing", inv.Missing,
			)
			return nil
		},
	})
	return inv
}

// provideScorer constructs the libvmaf cgo scorer from config and registers its
// Close as an OnStop hook. Because fx runs OnStop hooks in reverse of
// construction order, and the composition root forces the Scorer to be
// constructed before the gRPC server (see the R-node ordering invoke in main),
// this Close runs strictly AFTER the gRPC server's GracefulStop has drained
// in-flight Score / ScoreStream calls.
//
// The scorer is nil-tolerant: a missing vmaf binary is NON-FATAL — the node
// still serves Health (k8s liveness) and returns codes.FailedPrecondition from
// the scoring RPCs. R-node is forward-looking: libvmaf.Scorer.Close() is
// currently a no-op (the scorer shells out to the vmaf subprocess and holds no
// live C handle), so the ordering does not presently prevent a use-after-free.
func provideScorer(lc fx.Lifecycle, cfg *config.Config, log *slog.Logger) *libvmaf.Scorer {
	// golusoris env transform: VMAFX_VMAF_BINARY -> "vmaf.binary",
	// VMAFX_MODEL_DIR -> "model.dir". An unset vmaf.binary falls back to the
	// fork's FindBinary() search (installed / in-tree build).
	binary := cfg.Get("vmaf.binary")
	if binary == "" {
		binary = libvmaf.FindBinary()
	}
	modelDir := cfg.Get("model.dir")
	scorer, err := libvmaf.New(binary, modelDir)
	if err != nil {
		log.Warn("vmaf scorer unavailable — node serves Health only and rejects scoring RPCs",
			"error", err, "vmaf_binary", binary)
		return nil
	}
	log.Info("scorer initialised", "vmaf_binary", binary, "model_dir", modelDir)
	lc.Append(fx.Hook{
		OnStop: func(_ context.Context) error {
			log.Info("closing scorer (after gRPC drain)")
			scorer.Close()
			return nil
		},
	})
	return scorer
}

// provideExecutor builds the job executor that backs the controller-driven job
// pipeline. The AI registry is constructed from the model dir (nil-tolerant via
// the executor's own guards). backend is read from config (VMAFX_BACKEND ->
// "backend"), defaulting to "cpu".
func provideExecutor(scorer *libvmaf.Scorer, cfg *config.Config, log *slog.Logger) *Executor {
	backend := cfg.Get("backend")
	if backend == "" {
		backend = "cpu"
	}
	return NewExecutor(scorer, nil, backend, log)
}

// provideFeedbackClient constructs the online-training sidecar feedback client
// and binds its background drainer to the fx lifecycle: the drainer is started
// in OnStart (no goroutine spawned at construction) and Close()'d — which stops
// and awaits the drainer — in OnStop. This eliminates the prior pattern where
// NewFeedbackClient spawned a goroutine bound to a caller ctx, and guarantees a
// clean, leak-free shutdown.
//
// Because this provider is constructed after provideScorer and before the gRPC
// server (the R-node ordering), its OnStop fires AFTER the gRPC GracefulStop
// (RPCs that still want to Send feedback have drained) and BEFORE the scorer
// Close.
func provideFeedbackClient(lc fx.Lifecycle, cfg *config.Config, log *slog.Logger) *FeedbackClient {
	// golusoris env transform: VMAFX_SIDECAR_SOCKET -> "sidecar.socket". The
	// FeedbackClient still honours the bare VMAFX_SIDECAR_SOCKET os.Getenv path
	// for compatibility with the Python sidecar's own env contract; main resolves
	// the config value and exports it so both observe the same socket.
	if socket := cfg.Get("sidecar.socket"); socket != "" {
		_ = os.Setenv(feedbackSocketEnv, socket)
	}
	fc := NewFeedbackClient(log)
	lc.Append(fx.Hook{
		OnStart: func(_ context.Context) error {
			fc.Start()
			return nil
		},
		OnStop: func(_ context.Context) error {
			log.Info("stopping sidecar feedback drainer",
				"delivered", fc.Delivered(), "dropped", fc.Dropped())
			fc.Close()
			return nil
		},
	})
	return fc
}

// ffmpegBinary resolves the ffmpeg binary path from config. golusoris env
// transform: VMAFX_FFMPEG_BIN -> "ffmpeg.bin". Falls back to "ffmpeg" (PATH
// lookup); the node image bakes ffmpeg at /usr/local/bin/ffmpeg and sets
// VMAFX_FFMPEG_BIN accordingly (ADR-0717).
func ffmpegBinary(cfg *config.Config) string {
	if v := cfg.Get("ffmpeg.bin"); v != "" {
		return v
	}
	return "ffmpeg"
}
