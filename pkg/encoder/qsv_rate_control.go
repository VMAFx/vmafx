// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package encoder

import (
	"context"
	"fmt"
	"strings"

	"github.com/VMAFx/vmafx/pkg/corpus"
	"github.com/VMAFx/vmafx/pkg/hwdevice"
)

// prepareQSVRateControl uses the actual device chain's render node, including
// a chain supplied by a caller, and the existing injected FFmpeg runner.
func prepareQSVRateControl(ctx context.Context, params EncodeParams, codec string, argv []string, run corpus.Runner) ([]string, hwdevice.QSVRateControl, error) {
	if !hwdevice.NeedsQSVChain(codec) {
		return argv, hwdevice.QSVRateControl{}, nil
	}
	if params.CRF < 1 || params.CRF > 51 {
		return nil, hwdevice.QSVRateControl{}, fmt.Errorf("QSV quality %d outside range [1, 51]", params.CRF)
	}
	device := params.VAAPIDevice
	for _, arg := range params.InputArgs {
		if value, ok := strings.CutPrefix(arg, "vaapi=va:"); ok {
			device = value
		}
	}
	mode, err := hwdevice.ProbeQSVRateControl(ctx, device, codec, ffmpegBin(params), func(ctx context.Context, args []string) (string, int) {
		result := run(ctx, args)
		return result.Stderr + result.Stdout, result.ReturnCode
	})
	if err != nil {
		return nil, mode, err
	}
	argv, err = hwdevice.ApplyQSVRateControl(argv, params.CRF, mode)
	return argv, mode, err
}

func qsvEncodeMetadata(mode hwdevice.QSVRateControl) EncodeResult {
	result := EncodeResult{QSVRateControl: mode.Mode, QSVRateControlReason: mode.Reason}
	if mode.Mode != "" {
		result.Provenance = map[string]string{"qsv_rate_control": mode.Mode, "qsv_rate_control_reason": mode.Reason}
	}
	return result
}
