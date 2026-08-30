// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/hdr_test.go — HDR classification and codec-argv tests.
//
// The expected classifications and argv slices were produced by feeding the
// same ffprobe payloads through vmaftune.hdr._classify_payload and
// hdr_codec_args. HDR signalling that drifts between the two implementations
// changes what a corpus row's encode actually carried.

package corpus

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

// pqProbePayload is an ffprobe payload for a PQ HDR10 stream with full
// mastering-display and content-light side data.
const pqProbePayload = `{"streams": [{
	"color_transfer": "smpte2084",
	"color_primaries": "bt2020",
	"color_space": "bt2020nc",
	"color_range": "tv",
	"pix_fmt": "yuv420p10le",
	"side_data_list": [
		{
			"side_data_type": "Mastering display metadata",
			"red_x": "34000/50000", "red_y": "16000/50000",
			"green_x": "13250/50000", "green_y": "34500/50000",
			"blue_x": "7500/50000", "blue_y": "3000/50000",
			"white_point_x": "15635/50000", "white_point_y": "16450/50000",
			"min_luminance": "50/10000", "max_luminance": "10000000/10000"
		},
		{
			"side_data_type": "Content light level metadata",
			"max_content": 1000, "max_average": 400
		}
	]
}]}`

const hlgProbePayload = `{"streams": [{
	"color_transfer": "arib-std-b67",
	"color_primaries": "bt2020",
	"color_space": "bt2020nc",
	"color_range": "tv",
	"pix_fmt": "yuv420p10le",
	"side_data_list": []
}]}`

// mustClassify decodes a payload and classifies it.
func mustClassify(t *testing.T, payload string) *HdrInfo {
	t.Helper()
	var decoded map[string]any
	if err := json.Unmarshal([]byte(payload), &decoded); err != nil {
		t.Fatalf("fixture is not valid JSON: %v", err)
	}
	return ClassifyFFprobePayload(decoded)
}

func TestClassifyFFprobePayload(t *testing.T) {
	t.Parallel()

	t.Run("PQ with full side data", func(t *testing.T) {
		t.Parallel()
		got := mustClassify(t, pqProbePayload)
		if got == nil {
			t.Fatal("a PQ / BT.2020 stream classified as SDR")
		}
		want := &HdrInfo{
			Transfer: "pq", Primaries: "bt2020", Matrix: "bt2020nc",
			ColorRange: "tv", PixFmt: "yuv420p10le",
			MasterDisplay: "G(13250,34500)B(7500,3000)R(34000,16000)" +
				"WP(15635,16450)L(10000000,50)",
			MaxCLL: "1000,400",
		}
		if !reflect.DeepEqual(got, want) {
			t.Errorf("ClassifyFFprobePayload() = %+v, want %+v", got, want)
		}
	})

	t.Run("HLG without side data", func(t *testing.T) {
		t.Parallel()
		got := mustClassify(t, hlgProbePayload)
		if got == nil {
			t.Fatal("an HLG / BT.2020 stream classified as SDR")
		}
		if got.Transfer != "hlg" {
			t.Errorf("Transfer = %q, want hlg", got.Transfer)
		}
		if got.MasterDisplay != "" || got.MaxCLL != "" {
			t.Errorf("SEI payloads = (%q, %q), want empty without side data",
				got.MasterDisplay, got.MaxCLL)
		}
	})

	sdrCases := []struct {
		name    string
		payload string
	}{
		{name: "bt709 is SDR", payload: `{"streams": [{"color_transfer": "bt709",
			"color_primaries": "bt709"}]}`},
		{
			// A PQ transfer without BT.2020 primaries is malformed;
			// injecting PQ flags into a gamma-2.4 encode is the dangerous
			// failure mode, so it reads as SDR.
			name: "PQ with non-bt2020 primaries reads as SDR",
			payload: `{"streams": [{"color_transfer": "smpte2084",
				"color_primaries": "bt709"}]}`,
		},
		{name: "no streams", payload: `{}`},
		{name: "empty stream list", payload: `{"streams": []}`},
		{name: "missing colour metadata", payload: `{"streams": [{"pix_fmt": "yuv420p"}]}`},
	}
	for _, tc := range sdrCases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := mustClassify(t, tc.payload); got != nil {
				t.Errorf("ClassifyFFprobePayload() = %+v, want nil (SDR)", got)
			}
		})
	}
}

func TestHDRCodecArgs(t *testing.T) {
	t.Parallel()

	pq := mustClassify(t, pqProbePayload)
	hlg := mustClassify(t, hlgProbePayload)
	const masterDisplay = "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,50)"

	tests := []struct {
		name    string
		encoder string
		info    *HdrInfo
		want    []string
	}{
		{
			name:    "x264 gets container-level colour tags only",
			encoder: "libx264", info: pq,
			want: []string{
				"-color_primaries", "bt2020", "-color_trc", "smpte2084",
				"-colorspace", "bt2020nc", "-color_range", "tv",
			},
		},
		{
			name:    "x265 adds in-stream SEI via -x265-params",
			encoder: "libx265", info: pq,
			want: []string{
				"-color_primaries", "bt2020", "-color_trc", "smpte2084",
				"-colorspace", "bt2020nc", "-color_range", "tv",
				"-x265-params", "colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:" +
					"range=limited:master-display=" + masterDisplay +
					":max-cll=1000,400:hdr10-opt=1",
			},
		},
		{
			name:    "x265 HLG omits the hdr10-opt toggle and the absent SEI",
			encoder: "libx265", info: hlg,
			want: []string{
				"-color_primaries", "bt2020", "-color_trc", "arib-std-b67",
				"-colorspace", "bt2020nc", "-color_range", "tv",
				"-x265-params", "colorprim=bt2020:transfer=arib-std-b67:" +
					"colormatrix=bt2020nc:range=limited",
			},
		},
		{
			name:    "svt-av1 uses the AV1 enum values",
			encoder: "libsvtav1", info: pq,
			want: []string{
				"-color_primaries", "bt2020", "-color_trc", "smpte2084",
				"-colorspace", "bt2020nc", "-color_range", "tv",
				"-svtav1-params", "color-primaries=9:transfer-characteristics=16:" +
					"matrix-coefficients=9:color-range=0:mastering-display=" +
					masterDisplay + ":content-light=1000,400",
			},
		},
		{
			name:    "svt-av1 HLG uses transfer 18",
			encoder: "libsvtav1", info: hlg,
			want: []string{
				"-color_primaries", "bt2020", "-color_trc", "arib-std-b67",
				"-colorspace", "bt2020nc", "-color_range", "tv",
				"-svtav1-params", "color-primaries=9:transfer-characteristics=18:" +
					"matrix-coefficients=9:color-range=0",
			},
		},
		{
			name:    "nvenc hevc forces 10-bit and passes the SEI knobs",
			encoder: "hevc_nvenc", info: pq,
			want: []string{
				"-pix_fmt", "p010le", "-profile:v", "main10",
				"-color_primaries", "bt2020", "-color_trc", "smpte2084",
				"-colorspace", "bt2020nc", "-color_range", "tv",
				"-master_display", masterDisplay,
				"-max_cll", "1000,400",
			},
		},
		{
			name:    "qsv hevc forces 10-bit without the private SEI knobs",
			encoder: "hevc_qsv", info: pq,
			want: []string{
				"-pix_fmt", "p010le", "-profile:v", "main10",
				"-color_primaries", "bt2020", "-color_trc", "smpte2084",
				"-colorspace", "bt2020nc", "-color_range", "tv",
			},
		},
		{
			name:    "amf av1 forces 10-bit 4:2:0",
			encoder: "av1_amf", info: pq,
			want: []string{
				"-pix_fmt", "p010le",
				"-color_primaries", "bt2020", "-color_trc", "smpte2084",
				"-colorspace", "bt2020nc", "-color_range", "tv",
			},
		},
		{
			name:    "libaom gets colour tags only",
			encoder: "libaom-av1", info: pq,
			want: []string{
				"-color_primaries", "bt2020", "-color_trc", "smpte2084",
				"-colorspace", "bt2020nc", "-color_range", "tv",
			},
		},
		{
			name:    "libvvenc gets colour tags only",
			encoder: "libvvenc", info: pq,
			want: []string{
				"-color_primaries", "bt2020", "-color_trc", "smpte2084",
				"-colorspace", "bt2020nc", "-color_range", "tv",
			},
		},
		{
			// H.264 hardware encoders have no HDR dispatch: 8-bit AVC
			// cannot represent PQ, so emitting nothing is correct.
			name:    "an encoder with no HDR dispatch emits nothing",
			encoder: "h264_qsv", info: pq,
		},
		{name: "an SDR source emits nothing", encoder: "libx265"},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := HDRCodecArgs(tc.encoder, tc.info)
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("HDRCodecArgs(%s) =\n  %v\nwant\n  %v", tc.encoder, got, tc.want)
			}
		})
	}
}

func TestDetectHDR(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	src := filepath.Join(dir, "clip.mkv")
	if err := os.WriteFile(src, []byte("container"), 0o600); err != nil {
		t.Fatalf("seed source: %v", err)
	}

	t.Run("a successful probe classifies the stream", func(t *testing.T) {
		t.Parallel()
		var argv []string
		stub := func(_ context.Context, cmd []string) RunResult {
			argv = cmd
			return RunResult{Stdout: pqProbePayload}
		}
		got := DetectHDR(context.Background(), src, "ffprobe", stub)
		if got == nil || got.Transfer != "pq" {
			t.Fatalf("DetectHDR = %+v, want a PQ classification", got)
		}
		if argv[0] != "ffprobe" {
			t.Errorf("probe binary = %q, want ffprobe", argv[0])
		}
		joined := ""
		for _, a := range argv {
			joined += a + " "
		}
		for _, needle := range []string{"-select_streams", "v:0", "-show_streams", "-of", "json"} {
			if !containsArg(argv, needle) {
				t.Errorf("probe argv is missing %q: %s", needle, joined)
			}
		}
	})

	failures := []struct {
		name   string
		result RunResult
	}{
		{name: "a non-zero ffprobe exit", result: RunResult{ReturnCode: 1}},
		{name: "unparseable stdout", result: RunResult{Stdout: "not json"}},
		{name: "empty stdout", result: RunResult{}},
	}
	for _, tc := range failures {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			stub := func(context.Context, []string) RunResult { return tc.result }
			if got := DetectHDR(context.Background(), src, "ffprobe", stub); got != nil {
				t.Errorf("DetectHDR = %+v, want nil", got)
			}
		})
	}

	t.Run("a missing source is not probed", func(t *testing.T) {
		t.Parallel()
		called := false
		stub := func(context.Context, []string) RunResult {
			called = true
			return RunResult{Stdout: pqProbePayload}
		}
		if got := DetectHDR(context.Background(), filepath.Join(dir, "absent.mkv"),
			"ffprobe", stub); got != nil {
			t.Errorf("DetectHDR on a missing file = %+v, want nil", got)
		}
		if called {
			t.Error("DetectHDR spawned ffprobe for a missing file")
		}
	})
}

func containsArg(argv []string, needle string) bool {
	for _, a := range argv {
		if a == needle {
			return true
		}
	}
	return false
}

func TestHDRModelNameFor(t *testing.T) {
	t.Parallel()

	tests := []struct{ transfer, want string }{
		{transfer: "pq", want: HDRModelFilename},
		{transfer: "hlg", want: HDRModelFilename},
		{transfer: "PQ", want: HDRModelFilename},
		{transfer: "", want: ""},
		{transfer: "sdr", want: ""},
	}
	for _, tc := range tests {
		if got := HDRModelNameFor(tc.transfer); got != tc.want {
			t.Errorf("HDRModelNameFor(%q) = %q, want %q", tc.transfer, got, tc.want)
		}
	}
}

func TestSelectHDRVMAFModel(t *testing.T) {
	t.Parallel()

	t.Run("no model dir means no HDR model", func(t *testing.T) {
		t.Parallel()
		if got := SelectHDRVMAFModel(filepath.Join(t.TempDir(), "absent"), "pq"); got != "" {
			t.Errorf("SelectHDRVMAFModel = %q, want empty", got)
		}
	})

	t.Run("an empty model dir means no HDR model", func(t *testing.T) {
		t.Parallel()
		if got := SelectHDRVMAFModel(t.TempDir(), "pq"); got != "" {
			t.Errorf("SelectHDRVMAFModel = %q, want empty", got)
		}
	})

	t.Run("the canonical filename wins", func(t *testing.T) {
		t.Parallel()
		dir := t.TempDir()
		canonical := filepath.Join(dir, HDRModelFilename)
		other := filepath.Join(dir, "vmaf_hdr_v9.9.9.json")
		for _, p := range []string{canonical, other} {
			if err := os.WriteFile(p, []byte("{}"), 0o600); err != nil {
				t.Fatalf("seed model: %v", err)
			}
		}
		if got := SelectHDRVMAFModel(dir, "pq"); got != canonical {
			t.Errorf("SelectHDRVMAFModel = %q, want the canonical %q", got, canonical)
		}
	})

	t.Run("without the canonical name the newest glob match wins", func(t *testing.T) {
		t.Parallel()
		dir := t.TempDir()
		newest := filepath.Join(dir, "vmaf_hdr_v9.9.9.json")
		for _, p := range []string{filepath.Join(dir, "vmaf_hdr_v0.1.0.json"), newest} {
			if err := os.WriteFile(p, []byte("{}"), 0o600); err != nil {
				t.Fatalf("seed model: %v", err)
			}
		}
		if got := SelectHDRVMAFModel(dir, "pq"); got != newest {
			t.Errorf("SelectHDRVMAFModel = %q, want %q", got, newest)
		}
	})
}

func TestSyntheticHDRInfo(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name       string
		transfer   string
		pixFmt     string
		wantPixFmt string
	}{
		{
			name:     "an 8-bit source is forced to 10-bit",
			transfer: "pq", pixFmt: "yuv420p", wantPixFmt: "yuv420p10le",
		},
		{
			name:     "an already-10-bit source keeps its format",
			transfer: "hlg", pixFmt: "yuv422p10le", wantPixFmt: "yuv422p10le",
		},
		{
			name:     "a 12-bit source keeps its format",
			transfer: "pq", pixFmt: "yuv420p12le", wantPixFmt: "yuv420p12le",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := syntheticHDRInfo(tc.transfer, tc.pixFmt)
			if got.Transfer != tc.transfer {
				t.Errorf("Transfer = %q, want %q", got.Transfer, tc.transfer)
			}
			if got.PixFmt != tc.wantPixFmt {
				t.Errorf("PixFmt = %q, want %q", got.PixFmt, tc.wantPixFmt)
			}
			if got.Primaries != "bt2020" || got.Matrix != "bt2020nc" {
				t.Errorf("colour metadata = (%q, %q), want bt2020 / bt2020nc",
					got.Primaries, got.Matrix)
			}
			// Without ffprobe there is no way to read the SEI payloads, and
			// fabricating them would be worse than emitting none.
			if got.MasterDisplay != "" || got.MaxCLL != "" {
				t.Errorf("a forced HdrInfo fabricated SEI payloads: %q / %q",
					got.MasterDisplay, got.MaxCLL)
			}
		})
	}
}
