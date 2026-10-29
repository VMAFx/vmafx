// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package hwdevice

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// QSVRateControl records the quality knob and why CQP was selected.
type QSVRateControl struct {
	Mode   string `json:"qsv_rate_control"`
	Reason string `json:"qsv_rate_control_reason,omitempty"`
}

// Knob preserves ICQ by default; only a named fallback uses -q:v.
func (r QSVRateControl) Knob() string {
	if r.Mode == "cqp-fallback" {
		return "-q:v"
	}
	return "-global_quality"
}

// QSVProbeRunner adapts an existing FFmpeg runner without another exec helper.
type QSVProbeRunner func(context.Context, []string) (string, int)

type qsvProbeKey struct{ device, encoder, bin string }
type qsvProbeEntry struct {
	once   sync.Once
	result QSVRateControl
}

var qsvProbeCache sync.Map

// ProbeQSVRateControl probes eight synthetic frames once per device, encoder
// and FFmpeg runtime. The environment override bypasses the automatic cache.
func ProbeQSVRateControl(ctx context.Context, device, encoder, bin string, run QSVProbeRunner) (QSVRateControl, error) {
	override := os.Getenv("VMAFTUNE_QSV_RATE_CONTROL")
	switch override {
	case "", "auto":
	case "icq":
		return QSVRateControl{Mode: "icq"}, nil
	case "cqp":
		return QSVRateControl{Mode: "cqp-fallback", Reason: "VMAFTUNE_QSV_RATE_CONTROL=cqp"}, nil
	default:
		return QSVRateControl{}, fmt.Errorf("VMAFTUNE_QSV_RATE_CONTROL must be auto, icq or cqp")
	}
	if run == nil {
		return QSVRateControl{}, fmt.Errorf("QSV probe requires an FFmpeg runner")
	}
	device = ResolveVAAPIDevice(device)
	if resolved, err := filepath.EvalSymlinks(device); err == nil {
		device = resolved
	}
	if bin == "" {
		bin = "ffmpeg"
	}
	key := qsvProbeKey{device, encoder, bin}
	value, _ := qsvProbeCache.LoadOrStore(key, &qsvProbeEntry{})
	entry := value.(*qsvProbeEntry)
	entry.once.Do(func() {
		probeCtx, cancel := context.WithTimeout(ctx, 30*time.Second)
		defer cancel()
		argv := []string{bin, "-hide_banner", "-loglevel", "error"}
		argv = append(argv, QSVInitArgs(device)...)
		argv = append(argv, "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=24", "-frames:v", "8", "-vf", UploadFilter, "-c:v", encoder, "-preset", "veryfast", "-global_quality", "23", "-f", "null", "-")
		output, rc := run(probeCtx, argv)
		entry.result = QSVRateControl{Mode: "icq"}
		if rc != 0 {
			if len(output) > 1024 {
				output = output[len(output)-1024:]
			}
			entry.result = QSVRateControl{Mode: "cqp-fallback", Reason: fmt.Sprintf("ICQ probe failed (rc=%d): %s", rc, strings.TrimSpace(output))}
		}
	})
	return entry.result, nil
}

// ApplyQSVRateControl replaces the quality flag on a copied argv. Numeric
// quality stays unchanged and is validated identically in either mode.
func ApplyQSVRateControl(argv []string, quality int, mode QSVRateControl) ([]string, error) {
	if quality < 1 || quality > 51 {
		return nil, fmt.Errorf("QSV quality %d outside range [1, 51]", quality)
	}
	out := append([]string(nil), argv...)
	for i, flag := range out {
		if flag == "-global_quality" {
			out[i] = mode.Knob()
			return out, nil
		}
	}
	return nil, fmt.Errorf("QSV argv has no -global_quality knob")
}
