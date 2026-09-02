// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package corpusrow_test

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/corpusrow"
	"github.com/VMAFx/vmafx/pkg/ffencode"
	"github.com/VMAFx/vmafx/pkg/scorecli"
)

// pythonSchemaV3Keys is CORPUS_ROW_KEYS dumped from
// tools/vmaf-tune/src/vmaftune/corpus.py. The Go row must carry exactly this
// key set or a downstream reader of the JSONL breaks.
var pythonSchemaV3Keys = []string{
	"adm2_mean", "adm2_std", "bitrate_kbps", "clip_mode", "crf", "duration_s",
	"enc_internal_bits_mean", "enc_internal_bits_std", "enc_internal_intra_ratio",
	"enc_internal_itex_mean", "enc_internal_mv_mean", "enc_internal_mv_std",
	"enc_internal_ptex_mean", "enc_internal_qp_mean", "enc_internal_qp_std",
	"enc_internal_skip_ratio", "encode_path", "encode_size_bytes",
	"encode_time_ms", "encoder", "encoder_version", "exit_status",
	"extra_params", "ffmpeg_version", "framerate", "hdr_forced",
	"hdr_primaries", "hdr_transfer", "height", "motion2_mean", "motion2_std",
	"pix_fmt", "preset", "run_id", "schema_version", "score_time_ms",
	"shot_avg_duration_sec", "shot_count", "shot_duration_std_sec", "src",
	"src_sha256", "timestamp", "vif_scale0_mean", "vif_scale0_std",
	"vif_scale1_mean", "vif_scale1_std", "vif_scale2_mean", "vif_scale2_std",
	"vif_scale3_mean", "vif_scale3_std", "vmaf_binary_version", "vmaf_model",
	"vmaf_score", "width",
}

// TestKeys_matchPythonSchemaV3 is the schema gate: a key added, dropped or
// renamed on either side fails here rather than silently at read time.
func TestKeys_matchPythonSchemaV3(t *testing.T) {
	t.Parallel()

	got := corpusrow.Keys()
	if !slices.Equal(got, pythonSchemaV3Keys) {
		t.Errorf("row key set mismatch\n got (%d): %v\nwant (%d): %v",
			len(got), got, len(pythonSchemaV3Keys), pythonSchemaV3Keys)
	}
}

// TestNewRow covers the value mapping, including the two contracts that
// matter to consumers: an absent feature aggregate is NaN (not a synthetic
// zero), and the exit status is the encode's unless that succeeded.
func TestNewRow(t *testing.T) {
	t.Parallel()

	job := corpusrow.Job{
		Source: "/src/a.yuv", Width: 1920, Height: 1080,
		PixFmt: "yuv420p", Framerate: 24.0, DurationS: 10.0,
		SrcSHA256: "abc123",
	}
	opts := corpusrow.Options{
		Encoder: "libx264", VMAFModel: "vmaf_v0.6.1", KeepEncodes: true,
	}
	enc := ffencode.Result{
		Request: ffencode.Request{
			Output: "/enc/out.mp4", ExtraParams: []string{"-vf", "scale=1280:720"},
		},
		EncodeSizeBytes: 1_000_000,
		EncodeTimeMS:    1234.5,
		EncoderVersion:  "libx264-164",
		FFmpegVersion:   "8.1",
	}
	score := scorecli.Result{
		VMAFScore: 93.25, ScoreTimeMS: 456.7, VMAFBinaryVersion: "3.0.0",
		FeatureMeans: map[string]float64{"adm2": 0.97, "motion2": 3.5},
		FeatureStds:  map[string]float64{"adm2": 0.01},
	}

	row := corpusrow.NewRow(job, opts, "medium", 23, enc, score, "vmaf_v0.6.1")

	if row["schema_version"] != corpusrow.SchemaVersion {
		t.Errorf("schema_version = %v, want %v", row["schema_version"], corpusrow.SchemaVersion)
	}
	if row["src"] != "/src/a.yuv" || row["preset"] != "medium" || row["crf"] != 23 {
		t.Errorf("identity fields wrong: %v / %v / %v", row["src"], row["preset"], row["crf"])
	}
	// 1 MB over 10 s = 800 kbps.
	if got := row["bitrate_kbps"].(float64); math.Abs(got-800.0) > 1e-9 {
		t.Errorf("bitrate_kbps = %v, want 800", got)
	}
	if row["encode_path"] != "/enc/out.mp4" {
		t.Errorf("encode_path = %v, want the output path when keep-encodes is on",
			row["encode_path"])
	}
	if got := row["adm2_mean"].(float64); got != 0.97 {
		t.Errorf("adm2_mean = %v, want 0.97", got)
	}
	// motion2 has a mean but no stddev; the std column must be NaN.
	if got := row["motion2_std"].(float64); !math.IsNaN(got) {
		t.Errorf("motion2_std = %v, want NaN for an absent aggregate", got)
	}
	// vif_scale0 was never emitted; both columns must be NaN.
	if got := row["vif_scale0_mean"].(float64); !math.IsNaN(got) {
		t.Errorf("vif_scale0_mean = %v, want NaN for an unmeasured feature", got)
	}
	if got := row["run_id"].(string); len(got) != 32 {
		t.Errorf("run_id = %q, want 32 hex characters", got)
	}
	for _, key := range []string{"enc_internal_qp_mean", "enc_internal_skip_ratio"} {
		if got := row[key].(float64); got != 0.0 {
			t.Errorf("%s = %v, want 0.0 (encoder stats are corpus-group scope)", key, got)
		}
	}
}

// TestNewRow_keepEncodesOff asserts the encode path is withheld when the
// artefact is going to be deleted.
func TestNewRow_keepEncodesOff(t *testing.T) {
	t.Parallel()

	row := corpusrow.NewRow(
		corpusrow.Job{DurationS: 1}, corpusrow.Options{KeepEncodes: false},
		"medium", 23,
		ffencode.Result{Request: ffencode.Request{Output: "/enc/out.mp4"}},
		scorecli.Result{}, "vmaf_v0.6.1")

	if row["encode_path"] != "" {
		t.Errorf("encode_path = %v, want empty when keep-encodes is off", row["encode_path"])
	}
}

// TestNewRow_exitStatus asserts the encode's failure wins, and the score's is
// used only when the encode succeeded.
func TestNewRow_exitStatus(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name        string
		encStatus   int
		scoreStatus int
		want        int
	}{
		{"both clean", 0, 0, 0},
		{"encode failed", 1, 0, 1},
		{"score failed", 0, 65, 65},
		{"encode failure wins", 1, 65, 1},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			row := corpusrow.NewRow(
				corpusrow.Job{DurationS: 1}, corpusrow.Options{}, "medium", 23,
				ffencode.Result{ExitStatus: tc.encStatus},
				scorecli.Result{ExitStatus: tc.scoreStatus}, "vmaf_v0.6.1")
			if row["exit_status"] != tc.want {
				t.Errorf("exit_status = %v, want %d", row["exit_status"], tc.want)
			}
		})
	}
}

// TestCoarseGridCRFs pins the grid against the Python coarse_grid_crfs.
func TestCoarseGridCRFs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		min     int
		max     int
		step    int
		want    []int
		wantErr bool
	}{
		{"defaults", 10, 50, 10, []int{10, 20, 30, 40, 50}, false},
		{"step 5", 10, 30, 5, []int{10, 15, 20, 25, 30}, false},
		{"step wider than the range", 10, 15, 100, []int{10}, false},
		{"clamped above 51", 45, 60, 10, []int{45, 51}, false},
		{"single point", 23, 23, 1, []int{23}, false},
		{"zero step", 10, 50, 0, nil, true},
		{"negative step", 10, 50, -1, nil, true},
		{"inverted range", 50, 10, 10, nil, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := corpusrow.CoarseGridCRFs(tc.min, tc.max, tc.step)
			if (err != nil) != tc.wantErr {
				t.Fatalf("CoarseGridCRFs error = %v, wantErr %v", err, tc.wantErr)
			}
			if !tc.wantErr && !slices.Equal(got, tc.want) {
				t.Errorf("CoarseGridCRFs = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestFineGridCRFs pins the refinement grid against the Python
// fine_grid_crfs, including the exclusion of already-measured points.
func TestFineGridCRFs(t *testing.T) {
	t.Parallel()

	coarse := []int{10, 20, 30, 40, 50}

	tests := []struct {
		name    string
		best    int
		radius  int
		step    int
		min     int
		max     int
		exclude []int
		want    []int
		wantErr bool
	}{
		{
			name: "around the middle of the grid",
			best: 30, radius: 5, step: 1, min: 10, max: 50, exclude: coarse,
			want: []int{25, 26, 27, 28, 29, 31, 32, 33, 34, 35},
		},
		{
			name: "at the top of the grid with step 2",
			best: 50, radius: 5, step: 2, min: 10, max: 50, exclude: coarse,
			want: []int{45, 47, 49},
		},
		{
			name: "range bounds clip the candidates",
			best: 12, radius: 5, step: 1, min: 10, max: 50, exclude: nil,
			want: []int{10, 11, 12, 13, 14, 15, 16, 17},
		},
		{
			name: "zero radius keeps only the centre",
			best: 30, radius: 0, step: 1, min: 10, max: 50, exclude: nil,
			want: []int{30},
		},
		{
			name: "zero radius on an excluded centre is empty",
			best: 30, radius: 0, step: 1, min: 10, max: 50, exclude: coarse,
			want: []int{},
		},
		{
			name: "negative radius", best: 30, radius: -1, step: 1,
			min: 10, max: 50, wantErr: true,
		},
		{
			name: "zero step", best: 30, radius: 5, step: 0,
			min: 10, max: 50, wantErr: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := corpusrow.FineGridCRFs(
				tc.best, tc.radius, tc.step, tc.min, tc.max, tc.exclude)
			if (err != nil) != tc.wantErr {
				t.Fatalf("FineGridCRFs error = %v, wantErr %v", err, tc.wantErr)
			}
			if !tc.wantErr && !slices.Equal(got, tc.want) {
				t.Errorf("FineGridCRFs = %v, want %v", got, tc.want)
			}
		})
	}
}

// mkRow builds a minimal row for the picker tests.
func mkRow(crf int, score float64) corpusrow.Row {
	return corpusrow.Row{"crf": crf, "vmaf_score": score}
}

// TestPickBestCRF covers the with-target and without-target rules and the
// NaN filtering.
func TestPickBestCRF(t *testing.T) {
	t.Parallel()

	f := func(v float64) *float64 { return &v }

	tests := []struct {
		name    string
		rows    []corpusrow.Row
		target  *float64
		wantCRF int
		wantOK  bool
	}{
		{
			name: "with a target, the highest passing CRF wins",
			rows: []corpusrow.Row{
				mkRow(10, 99), mkRow(20, 96), mkRow(30, 93.5), mkRow(40, 88),
			},
			target: f(93.0), wantCRF: 30, wantOK: true,
		},
		{
			name:   "nothing passes, fall back to the highest VMAF",
			rows:   []corpusrow.Row{mkRow(30, 88), mkRow(40, 85)},
			target: f(93.0), wantCRF: 30, wantOK: true,
		},
		{
			name:   "without a target, the highest VMAF wins",
			rows:   []corpusrow.Row{mkRow(30, 88), mkRow(10, 99), mkRow(20, 96)},
			target: nil, wantCRF: 10, wantOK: true,
		},
		{
			name: "NaN rows are ignored",
			rows: []corpusrow.Row{
				mkRow(10, math.NaN()), mkRow(20, 96), mkRow(30, math.NaN()),
			},
			target: f(93.0), wantCRF: 20, wantOK: true,
		},
		{
			name: "every row is NaN", rows: []corpusrow.Row{mkRow(10, math.NaN())},
			target: f(93.0), wantOK: false,
		},
		{name: "no rows", rows: nil, target: f(93.0), wantOK: false},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			gotCRF, gotOK := corpusrow.PickBestCRF(tc.rows, tc.target)
			if gotOK != tc.wantOK {
				t.Fatalf("ok = %v, want %v", gotOK, tc.wantOK)
			}
			if gotOK && gotCRF != tc.wantCRF {
				t.Errorf("crf = %d, want %d", gotCRF, tc.wantCRF)
			}
		})
	}
}

// TestShouldSkipRefinement covers each skip condition.
func TestShouldSkipRefinement(t *testing.T) {
	t.Parallel()

	f := func(v float64) *float64 { return &v }
	coarse := []int{10, 20, 30, 40, 50}

	tests := []struct {
		name      string
		bestCRF   int
		haveBest  bool
		target    *float64
		bestScore float64
		crfMax    int
		want      bool
	}{
		{
			name: "no measurable coarse rows", haveBest: false,
			target: f(93), crfMax: 50, want: true,
		},
		{
			name: "no target always refines", bestCRF: 30, haveBest: true,
			target: nil, bestScore: 95, crfMax: 50, want: false,
		},
		{
			name: "NaN best score refines", bestCRF: 30, haveBest: true,
			target: f(93), bestScore: math.NaN(), crfMax: 50, want: false,
		},
		{
			name: "target missed refines", bestCRF: 30, haveBest: true,
			target: f(93), bestScore: 88, crfMax: 50, want: false,
		},
		{
			name: "target met mid-grid refines", bestCRF: 30, haveBest: true,
			target: f(93), bestScore: 95, crfMax: 50, want: false,
		},
		{
			name: "target met at the top of the grid skips", bestCRF: 50,
			haveBest: true, target: f(93), bestScore: 95, crfMax: 50, want: true,
		},
		{
			name: "target met pinned at crfMax skips", bestCRF: 45,
			haveBest: true, target: f(93), bestScore: 95, crfMax: 45, want: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got := corpusrow.ShouldSkipRefinement(
				tc.bestCRF, tc.haveBest, coarse, tc.target, tc.bestScore, tc.crfMax)
			if got != tc.want {
				t.Errorf("ShouldSkipRefinement = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestCoarseToFineSearch drives the two-pass loop through a stub cell runner,
// asserting the visited CRFs and the encode-count saving the design promises.
func TestCoarseToFineSearch(t *testing.T) {
	t.Parallel()

	f := func(v float64) *float64 { return &v }

	// A synthetic quality curve: VMAF falls linearly with CRF, crossing 93
	// between CRF 28 and 29.
	scoreFor := func(crf int) float64 { return 121.0 - float64(crf) }

	t.Run("coarse then fine", func(t *testing.T) {
		t.Parallel()

		var visitedCRFs []int
		run := func(_ context.Context, preset string, crf int) (corpusrow.Row, error) {
			visitedCRFs = append(visitedCRFs, crf)
			return corpusrow.Row{"crf": crf, "vmaf_score": scoreFor(crf), "preset": preset}, nil
		}
		opts := corpusrow.DefaultSearchOptions()
		opts.Presets = []string{"medium"}
		opts.TargetVMAF = f(93.0)

		rows, err := corpusrow.CoarseToFineSearch(context.Background(), run, opts)
		if err != nil {
			t.Fatalf("CoarseToFineSearch: %v", err)
		}
		// Coarse 10,20,30,40,50 -> best passing CRF is 20 (score 101 >= 93;
		// 30 scores 91). Fine pass then probes 15..25 minus the coarse grid.
		wantCRFs := []int{
			10, 20, 30, 40, 50,
			15, 16, 17, 18, 19, 21, 22, 23, 24, 25,
		}
		if !slices.Equal(visitedCRFs, wantCRFs) {
			t.Errorf("visited CRFs = %v, want %v", visitedCRFs, wantCRFs)
		}
		if len(rows) != len(wantCRFs) {
			t.Errorf("row count = %d, want %d", len(rows), len(wantCRFs))
		}
		// The whole point of coarse-to-fine: far fewer than a full sweep.
		if len(rows) >= 42 {
			t.Errorf("visited %d cells, no better than a full 10..51 sweep", len(rows))
		}
	})

	t.Run("skips refinement at the top of the grid", func(t *testing.T) {
		t.Parallel()

		var visitedCRFs []int
		run := func(_ context.Context, _ string, crf int) (corpusrow.Row, error) {
			visitedCRFs = append(visitedCRFs, crf)
			// Everything clears the target, so the best passing CRF is 50.
			return corpusrow.Row{"crf": crf, "vmaf_score": 99.0}, nil
		}
		opts := corpusrow.DefaultSearchOptions()
		opts.Presets = []string{"medium"}
		opts.TargetVMAF = f(93.0)

		if _, err := corpusrow.CoarseToFineSearch(
			context.Background(), run, opts); err != nil {
			t.Fatalf("CoarseToFineSearch: %v", err)
		}
		if !slices.Equal(visitedCRFs, []int{10, 20, 30, 40, 50}) {
			t.Errorf("visited %v, want the coarse grid only", visitedCRFs)
		}
	})

	t.Run("runs once per distinct preset", func(t *testing.T) {
		t.Parallel()

		presetCounts := map[string]int{}
		run := func(_ context.Context, preset string, crf int) (corpusrow.Row, error) {
			presetCounts[preset]++
			return corpusrow.Row{"crf": crf, "vmaf_score": 99.0}, nil
		}
		opts := corpusrow.DefaultSearchOptions()
		opts.Presets = []string{"medium", "slow", "medium"}
		opts.TargetVMAF = f(93.0)

		if _, err := corpusrow.CoarseToFineSearch(
			context.Background(), run, opts); err != nil {
			t.Fatalf("CoarseToFineSearch: %v", err)
		}
		if presetCounts["medium"] != 5 || presetCounts["slow"] != 5 {
			t.Errorf("preset cell counts = %v, want 5 each with no duplicate sweep",
				presetCounts)
		}
	})

	t.Run("no presets is a no-op", func(t *testing.T) {
		t.Parallel()

		rows, err := corpusrow.CoarseToFineSearch(context.Background(),
			func(context.Context, string, int) (corpusrow.Row, error) {
				t.Error("the runner should not be called")
				return nil, nil
			}, corpusrow.DefaultSearchOptions())
		if err != nil {
			t.Fatalf("CoarseToFineSearch: %v", err)
		}
		if len(rows) != 0 {
			t.Errorf("row count = %d, want 0", len(rows))
		}
	})

	t.Run("a cell failure aborts", func(t *testing.T) {
		t.Parallel()

		opts := corpusrow.DefaultSearchOptions()
		opts.Presets = []string{"medium"}
		_, err := corpusrow.CoarseToFineSearch(context.Background(),
			func(context.Context, string, int) (corpusrow.Row, error) {
				return nil, errors.New("ffmpeg died")
			}, opts)
		if err == nil {
			t.Fatal("expected the cell failure to abort the sweep")
		}
	})
}

// TestWriteJSONL round-trips rows through the on-disk format.
func TestWriteJSONL(t *testing.T) {
	t.Parallel()

	path := filepath.Join(t.TempDir(), "nested", "corpus.jsonl")
	rows := []corpusrow.Row{
		{"crf": 20, "vmaf_score": 96.0},
		{"crf": 24, "vmaf_score": 93.5},
	}
	n, err := corpusrow.WriteJSONL(rows, path)
	if err != nil {
		t.Fatalf("WriteJSONL: %v", err)
	}
	if n != 2 {
		t.Errorf("wrote %d rows, want 2", n)
	}

	data, readErr := os.ReadFile(path) // #nosec G304 -- test-controlled path
	if readErr != nil {
		t.Fatalf("read corpus: %v", readErr)
	}
	lines := strings.Split(strings.TrimRight(string(data), "\n"), "\n")
	if len(lines) != 2 {
		t.Fatalf("wrote %d lines, want 2", len(lines))
	}
	for i, line := range lines {
		var decoded map[string]any
		if unmarshalErr := json.Unmarshal([]byte(line), &decoded); unmarshalErr != nil {
			t.Errorf("line %d is not valid JSON: %v", i, unmarshalErr)
		}
	}
	// The line must be byte-identical to Python's
	// json.dumps(row, sort_keys=True): sorted keys, ", " / ": " separators,
	// and a trailing ".0" on whole-valued floats.
	if lines[0] != `{"crf": 20, "vmaf_score": 96.0}` {
		t.Errorf("line 0 = %q, want the Python json.dumps byte shape", lines[0])
	}
}

// TestMarshalRow_matchesPythonJSONDumps pins the writer against
// json.dumps(row, sort_keys=True) output for the shapes the schema carries,
// including the non-finite tokens that plain encoding/json cannot produce.
func TestMarshalRow_matchesPythonJSONDumps(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		row  corpusrow.Row
		want string
	}{
		{
			name: "sorted keys with Python separators",
			row:  corpusrow.Row{"vmaf_score": 96.0, "crf": 20},
			want: `{"crf": 20, "vmaf_score": 96.0}`,
		},
		{
			name: "NaN is a bare token, not null",
			row: corpusrow.Row{
				"adm2_mean": math.NaN(), "bitrate_kbps": 800.0, "crf": 20,
			},
			want: `{"adm2_mean": NaN, "bitrate_kbps": 800.0, "crf": 20}`,
		},
		{
			name: "infinities carry Python's spelling",
			row:  corpusrow.Row{"hi": math.Inf(1), "lo": math.Inf(-1)},
			want: `{"hi": Infinity, "lo": -Infinity}`,
		},
		{
			name: "fractional floats keep their digits",
			row:  corpusrow.Row{"vmaf_score": 93.25},
			want: `{"vmaf_score": 93.25}`,
		},
		{
			name: "string arrays use the same element separator",
			row:  corpusrow.Row{"extra_params": []string{"-vf", "scale=1280:720"}},
			want: `{"extra_params": ["-vf", "scale=1280:720"]}`,
		},
		{
			name: "empty array",
			row:  corpusrow.Row{"extra_params": []string{}},
			want: `{"extra_params": []}`,
		},
		{
			name: "booleans and strings",
			row:  corpusrow.Row{"hdr_forced": false, "clip_mode": "full"},
			want: `{"clip_mode": "full", "hdr_forced": false}`,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := corpusrow.MarshalRow(tc.row)
			if err != nil {
				t.Fatalf("MarshalRow: %v", err)
			}
			if got != tc.want {
				t.Errorf("MarshalRow =\n %s\nwant\n %s", got, tc.want)
			}
		})
	}
}

// TestWriteJSONL_survivesNaNAggregates is the regression guard for the bug a
// live run caught: encoding/json refuses to marshal NaN, so a corpus row with
// an unmeasured feature aggregate aborted the whole sweep at write time.
func TestWriteJSONL_survivesNaNAggregates(t *testing.T) {
	t.Parallel()

	row := corpusrow.NewRow(
		corpusrow.Job{Source: "/a.yuv", DurationS: 2}, corpusrow.Options{},
		"medium", 23, ffencode.Result{}, scorecli.Result{VMAFScore: math.NaN()},
		"vmaf_v0.6.1")

	path := filepath.Join(t.TempDir(), "corpus.jsonl")
	if _, err := corpusrow.WriteJSONL([]corpusrow.Row{row}, path); err != nil {
		t.Fatalf("WriteJSONL with NaN aggregates: %v", err)
	}
	data, readErr := os.ReadFile(path) // #nosec G304 -- test-controlled path
	if readErr != nil {
		t.Fatalf("read corpus: %v", readErr)
	}
	if !strings.Contains(string(data), "NaN") {
		t.Error("the written row should carry bare NaN tokens for unmeasured features")
	}
}
