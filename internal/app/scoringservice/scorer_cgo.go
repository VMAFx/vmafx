// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build cgo

package scoringservice

import (
	"context"
	"fmt"
	"log/slog"

	"github.com/golusoris/golusoris/core/config"
	"go.uber.org/fx"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// ProvideScorer constructs the libvmaf scorer and closes it after the fx
// application's network servers have drained.
func ProvideScorer(
	lc fx.Lifecycle,
	cfg *config.Config,
	log *slog.Logger,
	buildVersion string,
) (*libvmaf.Scorer, error) {
	binary := cfg.Get("vmaf.binary")
	modelDir := cfg.Get("model.dir")
	scorer, err := libvmaf.New(binary, modelDir)
	if err != nil {
		return nil, fmt.Errorf("init scorer: %w", err)
	}
	log.Info("scorer initialised",
		"version", buildVersion,
		"vmaf_binary", binary,
		"model_dir", modelDir,
	)
	lc.Append(fx.Hook{
		OnStop: func(_ context.Context) error {
			log.Info("closing scorer (after gRPC drain)")
			scorer.Close()
			return nil
		},
	})
	return scorer, nil
}
