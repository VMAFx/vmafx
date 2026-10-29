// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package corpus

import (
	"context"
	"fmt"
	"strings"
	"testing"
)

func TestRunEncodeQSVRateControlRows(t *testing.T) {
	t.Setenv("VMAFTUNE_QSV_RATE_CONTROL", "auto")
	for _, rc := range []int{0, 183} {
		t.Run(fmt.Sprint(rc), func(t *testing.T) {
			var calls [][]string
			req := EncodeRequest{Source: "in.yuv", Output: t.TempDir() + "/out.mkv", Width: 320, Height: 240, PixFmt: "yuv420p", Framerate: 24, Encoder: "h264_qsv", Preset: "medium", CRF: 25, VAAPIDevice: "/dev/dri/test-" + t.Name()}
			run := func(ctx context.Context, argv []string) RunResult {
				calls = append(calls, argv)
				if strings.Contains(strings.Join(argv, " "), "testsrc2=") {
					return RunResult{ReturnCode: rc, Stderr: "Invalid FrameType:0"}
				}
				return RunResult{}
			}
			result := RunEncode(context.Background(), req, "ffmpeg", run)
			want, knob := "icq", "-global_quality"
			if rc != 0 {
				want, knob = "cqp-fallback", "-q:v"
			}
			if result.QSVRateControl != want || result.ExitStatus != 0 {
				t.Fatalf("result=%+v", result)
			}
			if len(calls) != 2 || !strings.Contains(strings.Join(calls[1], " "), knob+" 25") {
				t.Fatalf("calls=%v", calls)
			}
			row := buildRow(rowInput{Enc: result})
			if row["qsv_rate_control"] != want || row["provenance"].(map[string]string)["qsv_rate_control"] != want {
				t.Fatalf("row=%v", row)
			}
			if rc != 0 && row["qsv_rate_control_reason"] == "" {
				t.Fatal("missing fallback reason")
			}
		})
	}
}
