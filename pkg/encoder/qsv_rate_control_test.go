// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package encoder

import (
	"context"
	"fmt"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/corpus"
	"github.com/VMAFx/vmafx/pkg/hwdevice"
)

func TestQSVEncoderNamesForcedFallback(t *testing.T) {
	t.Setenv("VMAFTUNE_QSV_RATE_CONTROL", "cqp")
	for _, enc := range []Encoder{H264QSVEncoder{}, HEVCQSVEncoder{}} {
		t.Run(enc.Name(), func(t *testing.T) {
			calls := 0
			result, err := enc.Encode("in.mkv", EncodeParams{CRF: 25, OutputPath: t.TempDir() + "/out.mkv", FFmpegBin: "missing-ffmpeg", Runner: func(ctx context.Context, argv []string) corpus.RunResult {
				calls++
				if !strings.Contains(strings.Join(argv, " "), "-q:v 25") {
					t.Errorf("missing CQP knob: %v", argv)
				}
				if strings.Contains(strings.Join(argv, " "), "global_quality") {
					t.Errorf("unexpected ICQ knob: %v", argv)
				}
				return corpus.RunResult{}
			}})
			if err != nil {
				t.Fatal(err)
			}
			if calls != 1 || result.QSVRateControl != "cqp-fallback" || result.QSVRateControlReason == "" || result.Provenance == nil || result.Provenance.QSVRateControl != "cqp-fallback" {
				t.Fatalf("calls=%d result=%+v", calls, result)
			}
		})
	}
}

func TestQSVEncoderAutomaticProbe(t *testing.T) {
	t.Setenv("VMAFTUNE_QSV_RATE_CONTROL", "auto")
	for _, rc := range []int{0, 183} {
		t.Run(fmt.Sprint(rc), func(t *testing.T) {
			var calls [][]string
			enc := H264QSVEncoder{}
			params := EncodeParams{CRF: 1, VAAPIDevice: "/dev/dri/test-" + t.Name(), OutputPath: t.TempDir() + "/out.mkv", FFmpegBin: "missing-ffmpeg", Runner: func(ctx context.Context, argv []string) corpus.RunResult {
				calls = append(calls, argv)
				if strings.Contains(strings.Join(argv, " "), "testsrc2=") {
					return corpus.RunResult{ReturnCode: rc, Stderr: "Invalid FrameType:0"}
				}
				return corpus.RunResult{}
			}}
			for i := 0; i < 2; i++ {
				result, err := enc.Encode("in.mkv", params)
				if err != nil {
					t.Fatal(err)
				}
				want, knob := "icq", "-global_quality"
				if rc != 0 {
					want, knob = "cqp-fallback", "-q:v"
				}
				if result.QSVRateControl != want || !strings.Contains(strings.Join(calls[len(calls)-1], " "), knob+" 1") {
					t.Fatalf("result=%+v calls=%v", result, calls)
				}
			}
			if len(calls) != 3 {
				t.Fatalf("probe must be cached: calls=%v", calls)
			}
		})
	}
}

func TestFailedQSVEncodeRetainsFallback(t *testing.T) {
	t.Setenv("VMAFTUNE_QSV_RATE_CONTROL", "cqp")
	result, err := (H264QSVEncoder{}).Encode("in.mkv", EncodeParams{CRF: 25, OutputPath: t.TempDir() + "/out.mkv", Runner: func(context.Context, []string) corpus.RunResult {
		return corpus.RunResult{ReturnCode: 1, Stderr: "encoder unavailable"}
	}})
	if err == nil {
		t.Fatal("failed encode accepted")
	}
	if result.QSVRateControl != "cqp-fallback" || result.Provenance == nil || result.Provenance.QSVRateControlReason == "" {
		t.Fatalf("result=%+v", result)
	}
}

// TestEncodeResultComparable keeps EncodeResult usable with ==, which the Go
// API compatibility gate treats as part of the public API.
func TestEncodeResultComparable(t *testing.T) {
	a := qsvEncodeMetadata(hwdevice.QSVRateControl{Mode: "icq"})
	b := a
	if a != b {
		t.Fatal("copies of an EncodeResult compare unequal")
	}
}
