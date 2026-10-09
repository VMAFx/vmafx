// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package cmd

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/internal/execstub"
	"github.com/VMAFx/vmafx/pkg/corpus"
	"github.com/VMAFx/vmafx/pkg/encoder"
)

// ladderFakeTools writes a fake ffmpeg (writes a Y4M header of the requested
// scale to its last argument) and a fake vmaf (logs its argv, writes a pooled
// VMAF of 91.5 as XML). POSIX shell scripts.
func ladderFakeTools(t *testing.T) (dir, vmafLog string) {
	t.Helper()
	if runtime.GOOS == "windows" {
		t.Skip("the fake ffmpeg / vmaf are POSIX shell scripts")
	}
	dir = t.TempDir()
	vmafLog = filepath.Join(dir, "vmaf.log")
	ffmpeg := `#!/bin/sh
geom="64 36"
prev=""
for a in "$@"; do
  case "$prev" in -vf) geom=$(echo "$a" | sed 's/^scale=//; s/:/ /');; esac
  prev="$a"; last="$a"
done
set -- $geom
printf 'YUV4MPEG2 W%s H%s F24:1 C420jpeg\n' "$1" "$2" > "$last"
`
	vmaf := `#!/bin/sh
echo "vmaf $*" >> "` + vmafLog + `"
out=""
while [ $# -gt 0 ]; do
  case "$1" in
    --output) out="$2"; shift 2;;
    *) shift;;
  esac
done
echo '<metric name="vmaf" min="90" max="93" mean="91.5" harmonic_mean="91.4"/>' > "$out"
`
	for name, body := range map[string]string{"ffmpeg": ffmpeg, "vmaf": vmaf} {
		execstub.Write(t, filepath.Join(dir, name), []byte(body))
	}
	return dir, vmafLog
}

// TestLadderSampler_ModelPerRung is the regression for the Go ladder scoring
// every rung with libvmaf's default model: the Python ladder picks the model
// from each rung's own height (vmaftune.resolution.select_vmaf_model_version,
// ADR-0289), so a 2160-high rung uses the 4K model and every lower rung the
// default. Both languages read the same golden table
// (pkg/corpus/resolution_parity_test.go).
func TestLadderSampler_ModelPerRung(t *testing.T) {
	cases := []struct{ width, height int }{
		{3840, 2160}, {3840, 2159}, {1920, 1080}, {1280, 720}, {640, 480},
	}
	for _, tc := range cases {
		want, err := corpus.SelectVMAFModelVersion(tc.width, tc.height)
		if err != nil {
			t.Fatal(err)
		}
		t.Run(want+"/"+fmt.Sprintf("%dx%d", tc.width, tc.height), func(t *testing.T) {
			dir, vmafLog := ladderFakeTools(t)
			src := filepath.Join(t.TempDir(), "src.y4m")
			if err := os.WriteFile(src, []byte("YUV4MPEG2 W64 H36 F24:1 C420jpeg\n"), 0o600); err != nil {
				t.Fatal(err)
			}
			enc, err := encoder.NewExtended("libx264")
			if err != nil {
				t.Fatal(err)
			}
			flags := &ladderFlags{
				ffmpegBin: filepath.Join(dir, "ffmpeg"), vmafBin: filepath.Join(dir, "vmaf"),
				workDir: t.TempDir(), maxIter: 2,
			}
			if _, sampleErr := newLadderSampler(enc, flags)(src, "libx264", tc.width, tc.height, 90); sampleErr != nil {
				t.Fatalf("sampler: %v", sampleErr)
			}
			data, readErr := os.ReadFile(vmafLog)
			if readErr != nil {
				t.Fatalf("read vmaf log: %v", readErr)
			}
			calls := strings.Split(strings.TrimSpace(string(data)), "\n")
			if len(calls) == 0 {
				t.Fatal("the sampler never ran vmaf")
			}
			for _, call := range calls {
				if !strings.Contains(call, "--model version="+want+" ") && !strings.HasSuffix(call, "--model version="+want) {
					t.Errorf("%dx%d rung scored without --model version=%s: %q",
						tc.width, tc.height, want, call)
				}
			}
		})
	}
}
