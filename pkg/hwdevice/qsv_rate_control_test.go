// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package hwdevice

import (
	"context"
	"fmt"
	"reflect"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
)

func TestQSVRateControlProbeAndCache(t *testing.T) {
	t.Setenv("VMAFTUNE_QSV_RATE_CONTROL", "auto")
	for _, rc := range []int{0, 183} {
		t.Run(fmt.Sprint(rc), func(t *testing.T) {
			var calls atomic.Int32
			run := func(ctx context.Context, argv []string) (string, int) {
				calls.Add(1)
				if _, ok := ctx.Deadline(); !ok {
					t.Error("probe must have a deadline")
				}
				joined := strings.Join(argv, " ")
				for _, want := range []string{"testsrc2=size=320x240:rate=24", "-frames:v 8", "-global_quality 23", "-f null -", "qsv=qsv_dev@va"} {
					if !strings.Contains(joined, want) {
						t.Errorf("probe missing %s: %s", want, joined)
					}
				}
				return "Invalid FrameType:0", rc
			}
			device := fmt.Sprintf("/dev/dri/test-%s", t.Name())
			var wg sync.WaitGroup
			for range 16 {
				wg.Go(func() {
					result, err := ProbeQSVRateControl(context.Background(), device, "h264_qsv", "ffmpeg", run)
					if err != nil {
						t.Error(err)
						return
					}
					want, knob := "icq", "-global_quality"
					if rc != 0 {
						want, knob = "cqp-fallback", "-q:v"
					}
					if result.Mode != want || result.Knob() != knob {
						t.Errorf("unexpected result %+v", result)
					}
					if (result.Reason != "") != (rc != 0) {
						t.Errorf("fallback reason %+v", result)
					}
					argv, err := ApplyQSVRateControl([]string{"-c:v", "h264_qsv", "-global_quality", "51"}, 51, result)
					if err != nil || !reflect.DeepEqual(argv, []string{"-c:v", "h264_qsv", knob, "51"}) {
						t.Errorf("argv=%v err=%v", argv, err)
					}
				})
			}
			wg.Wait()
			if calls.Load() != 1 {
				t.Fatalf("probe calls=%d", calls.Load())
			}
			for _, key := range [][3]string{{device, "hevc_qsv", "ffmpeg"}, {device + "other", "h264_qsv", "ffmpeg"}, {device, "h264_qsv", "ffmpeg-other"}} {
				if _, err := ProbeQSVRateControl(context.Background(), key[0], key[1], key[2], run); err != nil {
					t.Fatal(err)
				}
			}
			if calls.Load() != 4 {
				t.Errorf("key isolation calls=%d", calls.Load())
			}
		})
	}
}

func TestQSVRateControlOverrides(t *testing.T) {
	for _, override := range []string{"icq", "cqp", "invalid"} {
		t.Run(override, func(t *testing.T) {
			t.Setenv("VMAFTUNE_QSV_RATE_CONTROL", override)
			result, err := ProbeQSVRateControl(context.Background(), "", "h264_qsv", "", func(context.Context, []string) (string, int) { t.Fatal("override ran probe"); return "", 0 })
			if override == "invalid" {
				if err == nil {
					t.Fatal("invalid override accepted")
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if override == "icq" && result.Mode != "icq" {
				t.Fatal(result)
			}
			if override == "cqp" && (result.Mode != "cqp-fallback" || result.Reason == "") {
				t.Fatal(result)
			}
		})
	}
}

func TestQSVRateControlQualityBounds(t *testing.T) {
	for _, quality := range []int{0, 1, 51, 52} {
		for _, mode := range []string{"icq", "cqp-fallback"} {
			_, err := ApplyQSVRateControl([]string{"-global_quality", fmt.Sprint(quality)}, quality, QSVRateControl{Mode: mode})
			if (err != nil) != (quality < 1 || quality > 51) {
				t.Errorf("quality=%d mode=%s err=%v", quality, mode, err)
			}
		}
	}
}
