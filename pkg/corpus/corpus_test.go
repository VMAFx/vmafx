// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/corpus_test.go — orchestrator tests.
//
// IterRows is driven through stubbed subprocess runners, the same seam the
// Python iter_rows exposes via encode_runner / score_runner. The row-shape
// assertions pin the v3 schema contract Phase B/C consume.

package corpus

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"
)

// crfFromArgv extracts the quality value from an ffmpeg argv, so a scripted
// runner can answer per-cell.
func crfFromArgv(argv []string) int {
	for i, a := range argv {
		switch a {
		case "-crf", "-cq", "-qp", "-global_quality", "-q:v":
			if i+1 < len(argv) {
				v, err := strconv.Atoi(argv[i+1])
				if err == nil {
					return v
				}
			}
		}
	}
	return -1
}

// scriptedRunners returns Runners that fake a whole sweep: the encoder writes a
// deterministic payload whose size varies with CRF, the scorer writes a
// libvmaf JSON carrying the scripted score for that CRF, and shot detection /
// ffprobe report "unavailable".
func scriptedRunners(scores map[int]float64) Runners {
	encode := func(_ context.Context, argv []string) RunResult {
		// The output path is the final argv token on a non-pass-1 encode.
		out := argv[len(argv)-1]
		if out == "-" {
			// Pass-1 stats invocation: nothing to write.
			return RunResult{Stderr: "x264 - core 164\n"}
		}
		crf := crfFromArgv(argv)
		payload := strings.Repeat("x", 1000+crf)
		if err := os.WriteFile(out, []byte(payload), 0o600); err != nil {
			return RunResult{ReturnCode: 1, Stderr: err.Error()}
		}
		return RunResult{Stderr: "ffmpeg version 7.1\nx264 - core 164\n"}
	}

	// The scorer needs to know which cell it is scoring; the distorted path
	// carries the CRF in its filename.
	score := func(_ context.Context, argv []string) RunResult {
		crf := -1
		for i, a := range argv {
			if a == "--distorted" && i+1 < len(argv) {
				base := filepath.Base(argv[i+1])
				if idx := strings.LastIndex(base, "crf"); idx >= 0 {
					digits := base[idx+3:]
					end := 0
					for end < len(digits) && digits[end] >= '0' && digits[end] <= '9' {
						end++
					}
					if v, err := strconv.Atoi(digits[:end]); err == nil {
						crf = v
					}
				}
			}
		}
		vmaf, ok := scores[crf]
		if !ok {
			vmaf = 90.0
		}
		body := `{"pooled_metrics": {"vmaf": {"mean": ` +
			strconv.FormatFloat(vmaf, 'f', -1, 64) +
			`}, "integer_adm2": {"mean": 0.98}, "integer_motion2": {"mean": 3.25}}}`
		for i, a := range argv {
			if a == "--output" && i+1 < len(argv) {
				if err := os.WriteFile(argv[i+1], []byte(body), 0o600); err != nil {
					return RunResult{ReturnCode: 1}
				}
			}
		}
		return RunResult{Stderr: "VMAF version 3.0.0\n"}
	}

	unavailable := func(context.Context, []string) RunResult {
		return RunResult{ReturnCode: 1}
	}

	return Runners{
		Encode: encode,
		Score:  score,
		Shot:   unavailable,
		Probe:  unavailable,
		Decode: unavailable,
	}
}

// rawYUVJob writes a small raw-YUV source so the reference leg needs no decode.
func rawYUVJob(t *testing.T, cells []Cell) (Job, Options) {
	t.Helper()
	dir := t.TempDir()
	src := filepath.Join(dir, "clip.yuv")
	if err := os.WriteFile(src, []byte("raw planar bytes"), 0o600); err != nil {
		t.Fatalf("seed source: %v", err)
	}
	job := Job{
		Source: src, Width: 320, Height: 240, PixFmt: "yuv420p",
		Framerate: 24.0, DurationS: 2.0, Cells: cells,
	}
	opts := NewOptions()
	opts.EncodeDir = filepath.Join(dir, "enc")
	opts.Output = filepath.Join(dir, "corpus.jsonl")
	opts.HDRMode = HDRModeForceSDR
	return job, opts
}

func TestIterRowsEmitsCompleteRows(t *testing.T) {
	t.Parallel()

	job, opts := rawYUVJob(t, []Cell{{Preset: "medium", CRF: 26}, {Preset: "medium", CRF: 32}})
	scores := map[int]float64{26: 96.5, 32: 91.25}

	var rows []map[string]any
	err := IterRows(context.Background(), job, opts, scriptedRunners(scores),
		func(row map[string]any) error {
			rows = append(rows, row)
			return nil
		})
	if err != nil {
		t.Fatalf("IterRows: %v", err)
	}
	if len(rows) != 2 {
		t.Fatalf("emitted %d rows, want 2", len(rows))
	}

	for i, row := range rows {
		if missing := MissingRowKeys(row); len(missing) > 0 {
			t.Errorf("row %d is missing schema keys: %v", i, missing)
		}
		if row["schema_version"] != SchemaVersion {
			t.Errorf("row %d schema_version = %v, want %d", i, row["schema_version"], SchemaVersion)
		}
		if row["exit_status"] != 0 {
			t.Errorf("row %d exit_status = %v, want 0", i, row["exit_status"])
		}
		runID, _ := row["run_id"].(string)
		if len(runID) != 32 {
			t.Errorf("row %d run_id = %q, want 32 hex chars", i, runID)
		}
		if ts, _ := row["timestamp"].(string); !strings.HasSuffix(ts, "+00:00") {
			t.Errorf("row %d timestamp = %q, want a numeric UTC offset", i, ts)
		}
	}

	if rows[0]["crf"] != 26 || rows[1]["crf"] != 32 {
		t.Errorf("cells were visited out of order: %v then %v", rows[0]["crf"], rows[1]["crf"])
	}
	if rows[0]["vmaf_score"] != 96.5 || rows[1]["vmaf_score"] != 91.25 {
		t.Errorf("scores = %v / %v, want 96.5 / 91.25",
			rows[0]["vmaf_score"], rows[1]["vmaf_score"])
	}
	// The canonical-6 columns the scorer emitted are real; the rest are NaN.
	if rows[0]["adm2_mean"] != 0.98 {
		t.Errorf("adm2_mean = %v, want 0.98", rows[0]["adm2_mean"])
	}
	if v, _ := rows[0]["vif_scale0_mean"].(float64); !math.IsNaN(v) {
		t.Errorf("vif_scale0_mean = %v, want NaN for a feature libvmaf did not emit", v)
	}
	if v, _ := rows[0]["adm2_std"].(float64); !math.IsNaN(v) {
		t.Errorf("adm2_std = %v, want NaN — the integer pipeline emits no stddev", v)
	}
}

func TestIterRowsBitrateUsesTheEncodedDuration(t *testing.T) {
	t.Parallel()

	job, opts := rawYUVJob(t, []Cell{{Preset: "medium", CRF: 26}})
	// Sample-clip mode encodes 1 s of a 2 s source; the bitrate must be
	// computed against the slice, not the full source.
	opts.SampleClipSeconds = 1.0

	var row map[string]any
	if err := IterRows(context.Background(), job, opts, scriptedRunners(nil),
		func(r map[string]any) error {
			row = r
			return nil
		}); err != nil {
		t.Fatalf("IterRows: %v", err)
	}
	if row["clip_mode"] != "sample_1s" {
		t.Errorf("clip_mode = %v, want sample_1s", row["clip_mode"])
	}
	size, _ := row["encode_size_bytes"].(int)
	want := BitrateKbps(int64(size), 1.0)
	if row["bitrate_kbps"] != want {
		t.Errorf("bitrate_kbps = %v, want %v (size over the 1 s slice)",
			row["bitrate_kbps"], want)
	}
	if row["duration_s"] != 2.0 {
		t.Errorf("duration_s = %v, want the source provenance 2.0", row["duration_s"])
	}
}

func TestIterRowsRecordsEncodeFailureAndSkipsScoring(t *testing.T) {
	t.Parallel()

	job, opts := rawYUVJob(t, []Cell{{Preset: "medium", CRF: 26}})
	runners := scriptedRunners(nil)
	runners.Encode = func(context.Context, []string) RunResult {
		return RunResult{ReturnCode: 42, Stderr: "encoder exploded\n"}
	}
	scoreCalled := false
	runners.Score = func(context.Context, []string) RunResult {
		scoreCalled = true
		return RunResult{}
	}

	var row map[string]any
	if err := IterRows(context.Background(), job, opts, runners,
		func(r map[string]any) error {
			row = r
			return nil
		}); err != nil {
		t.Fatalf("IterRows: %v", err)
	}
	if scoreCalled {
		t.Error("the scorer ran after a failed encode")
	}
	if row["exit_status"] != 42 {
		t.Errorf("exit_status = %v, want the encode status 42", row["exit_status"])
	}
	if v, _ := row["vmaf_score"].(float64); !math.IsNaN(v) {
		t.Errorf("vmaf_score = %v, want NaN", v)
	}
	if row["vmaf_binary_version"] != "skipped" {
		t.Errorf("vmaf_binary_version = %v, want skipped", row["vmaf_binary_version"])
	}
	if missing := MissingRowKeys(row); len(missing) > 0 {
		t.Errorf("a failed row is still missing schema keys: %v", missing)
	}
}

func TestIterRowsShortCircuitsOnReferenceDecodeFailure(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	// A container reference forces the once-per-sweep decode.
	src := filepath.Join(dir, "clip.mkv")
	if err := os.WriteFile(src, []byte("container"), 0o600); err != nil {
		t.Fatalf("seed source: %v", err)
	}
	job := Job{
		Source: src, Width: 320, Height: 240, PixFmt: "yuv420p",
		Framerate: 24.0, DurationS: 2.0,
		Cells: []Cell{{Preset: "medium", CRF: 26}, {Preset: "medium", CRF: 32}},
	}
	opts := NewOptions()
	opts.EncodeDir = filepath.Join(dir, "enc")
	opts.HDRMode = HDRModeForceSDR
	opts.SrcSHA256 = false

	runners := scriptedRunners(nil)
	encodeCalls := 0
	runners.Encode = func(context.Context, []string) RunResult {
		encodeCalls++
		return RunResult{}
	}
	runners.Decode = func(context.Context, []string) RunResult {
		return RunResult{ReturnCode: 9, Stderr: "decode failed\n"}
	}

	var rows []map[string]any
	if err := IterRows(context.Background(), job, opts, runners,
		func(r map[string]any) error {
			rows = append(rows, r)
			return nil
		}); err != nil {
		t.Fatalf("IterRows: %v", err)
	}
	if encodeCalls != 0 {
		t.Errorf("ran %d encodes after a failed reference decode, want 0", encodeCalls)
	}
	if len(rows) != 2 {
		t.Fatalf("emitted %d rows, want one per cell", len(rows))
	}
	for i, row := range rows {
		if row["exit_status"] != 9 {
			t.Errorf("row %d exit_status = %v, want the decode status 9", i, row["exit_status"])
		}
		if row["encoder_version"] != "skipped" {
			t.Errorf("row %d encoder_version = %v, want skipped", i, row["encoder_version"])
		}
		if missing := MissingRowKeys(row); len(missing) > 0 {
			t.Errorf("row %d is missing schema keys: %v", i, missing)
		}
	}
}

func TestIterRowsRejectsAnInvalidCell(t *testing.T) {
	t.Parallel()

	job, opts := rawYUVJob(t, []Cell{{Preset: "not-a-preset", CRF: 26}})
	err := IterRows(context.Background(), job, opts, scriptedRunners(nil),
		func(map[string]any) error { return nil })
	if err == nil {
		t.Fatal("IterRows accepted an invalid preset")
	}
}

func TestIterRowsPropagatesEmitErrors(t *testing.T) {
	t.Parallel()

	job, opts := rawYUVJob(t, []Cell{{Preset: "medium", CRF: 26}, {Preset: "medium", CRF: 32}})
	sentinel := errors.New("disk full")
	calls := 0
	err := IterRows(context.Background(), job, opts, scriptedRunners(nil),
		func(map[string]any) error {
			calls++
			return sentinel
		})
	if !errors.Is(err, sentinel) {
		t.Fatalf("IterRows error = %v, want the emit error", err)
	}
	if calls != 1 {
		t.Errorf("emit was called %d times, want 1 — the sweep should stop", calls)
	}
}

func TestIterRowsKeepEncodes(t *testing.T) {
	t.Parallel()

	t.Run("cleanup is the default", func(t *testing.T) {
		t.Parallel()
		job, opts := rawYUVJob(t, []Cell{{Preset: "medium", CRF: 26}})
		var row map[string]any
		if err := IterRows(context.Background(), job, opts, scriptedRunners(nil),
			func(r map[string]any) error {
				row = r
				return nil
			}); err != nil {
			t.Fatalf("IterRows: %v", err)
		}
		if row["encode_path"] != "" {
			t.Errorf("encode_path = %v, want empty when encodes are discarded",
				row["encode_path"])
		}
		out := encodePath(opts, job.Source, "medium", 26)
		if _, err := os.Stat(out); err == nil {
			t.Errorf("the encode at %q survived cleanup", out)
		}
	})

	t.Run("--keep-encodes retains the file and records its path", func(t *testing.T) {
		t.Parallel()
		job, opts := rawYUVJob(t, []Cell{{Preset: "medium", CRF: 26}})
		opts.KeepEncodes = true
		var row map[string]any
		if err := IterRows(context.Background(), job, opts, scriptedRunners(nil),
			func(r map[string]any) error {
				row = r
				return nil
			}); err != nil {
			t.Fatalf("IterRows: %v", err)
		}
		out := encodePath(opts, job.Source, "medium", 26)
		if row["encode_path"] != out {
			t.Errorf("encode_path = %v, want %q", row["encode_path"], out)
		}
		if _, err := os.Stat(out); err != nil {
			t.Errorf("the retained encode is missing: %v", err)
		}
	})
}

func TestIterRowsSourceHash(t *testing.T) {
	t.Parallel()

	job, opts := rawYUVJob(t, []Cell{{Preset: "medium", CRF: 26}})

	t.Run("hashing on", func(t *testing.T) {
		var row map[string]any
		if err := IterRows(context.Background(), job, opts, scriptedRunners(nil),
			func(r map[string]any) error {
				row = r
				return nil
			}); err != nil {
			t.Fatalf("IterRows: %v", err)
		}
		want, err := FileSHA256(job.Source)
		if err != nil {
			t.Fatalf("FileSHA256: %v", err)
		}
		if row["src_sha256"] != want {
			t.Errorf("src_sha256 = %v, want %q", row["src_sha256"], want)
		}
	})

	t.Run("--no-source-hash", func(t *testing.T) {
		noHash := opts
		noHash.SrcSHA256 = false
		var row map[string]any
		if err := IterRows(context.Background(), job, noHash, scriptedRunners(nil),
			func(r map[string]any) error {
				row = r
				return nil
			}); err != nil {
			t.Fatalf("IterRows: %v", err)
		}
		if row["src_sha256"] != "" {
			t.Errorf("src_sha256 = %v, want empty", row["src_sha256"])
		}
	})
}

func TestIterRowsHDRProvenance(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name           string
		hdrMode        string
		wantTransfer   string
		wantPrimaries  string
		wantForced     bool
		wantColorFlags bool
	}{
		{name: "force-sdr", hdrMode: HDRModeForceSDR, wantForced: true},
		{
			name: "force-hdr-pq", hdrMode: HDRModeForcePQ,
			wantTransfer: "pq", wantPrimaries: "bt2020",
			wantForced: true, wantColorFlags: true,
		},
		{
			name: "force-hdr-hlg", hdrMode: HDRModeForceHLG,
			wantTransfer: "hlg", wantPrimaries: "bt2020",
			wantForced: true, wantColorFlags: true,
		},
		{
			// auto with a failing ffprobe reads as SDR, and the row records
			// that detection (not the user) made the call.
			name: "auto with no probe", hdrMode: HDRModeAuto,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			job, opts := rawYUVJob(t, []Cell{{Preset: "medium", CRF: 26}})
			opts.HDRMode = tc.hdrMode

			var row map[string]any
			var encodeArgv []string
			runners := scriptedRunners(nil)
			base := runners.Encode
			runners.Encode = func(ctx context.Context, argv []string) RunResult {
				encodeArgv = argv
				return base(ctx, argv)
			}

			if err := IterRows(context.Background(), job, opts, runners,
				func(r map[string]any) error {
					row = r
					return nil
				}); err != nil {
				t.Fatalf("IterRows: %v", err)
			}
			if row["hdr_transfer"] != tc.wantTransfer {
				t.Errorf("hdr_transfer = %v, want %q", row["hdr_transfer"], tc.wantTransfer)
			}
			if row["hdr_primaries"] != tc.wantPrimaries {
				t.Errorf("hdr_primaries = %v, want %q", row["hdr_primaries"], tc.wantPrimaries)
			}
			if row["hdr_forced"] != tc.wantForced {
				t.Errorf("hdr_forced = %v, want %v", row["hdr_forced"], tc.wantForced)
			}
			hasColorFlags := false
			for _, a := range encodeArgv {
				if a == "-color_trc" {
					hasColorFlags = true
				}
			}
			if hasColorFlags != tc.wantColorFlags {
				t.Errorf("encode argv carried colour flags = %v, want %v",
					hasColorFlags, tc.wantColorFlags)
			}
		})
	}
}

func TestIterRowsResolutionAwareModelSelection(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name            string
		width, height   int
		resolutionAware bool
		configuredModel string
		want            string
	}{
		{name: "HD picks the 1080p model", width: 1920, height: 1080,
			resolutionAware: true, configuredModel: Model1080P, want: Model1080P},
		{name: "UHD picks the 4K model", width: 3840, height: 2160,
			resolutionAware: true, configuredModel: Model1080P, want: Model4K},
		{
			// Parity note: resolution-aware selection overrides the
			// configured model, so --neg is silently dropped. The Python
			// pipeline behaves identically; the Go port reproduces it
			// rather than diverging.
			name:  "resolution-aware selection overrides an explicit model",
			width: 1920, height: 1080, resolutionAware: true,
			configuredModel: Model1080PNEG, want: Model1080P,
		},
		{
			name:  "with resolution-awareness off the configured model survives",
			width: 3840, height: 2160, resolutionAware: false,
			configuredModel: Model1080PNEG, want: Model1080PNEG,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			job, opts := rawYUVJob(t, []Cell{{Preset: "medium", CRF: 26}})
			job.Width, job.Height = tc.width, tc.height
			opts.ResolutionAware = tc.resolutionAware
			opts.VMAFModel = tc.configuredModel

			var row map[string]any
			if err := IterRows(context.Background(), job, opts, scriptedRunners(nil),
				func(r map[string]any) error {
					row = r
					return nil
				}); err != nil {
				t.Fatalf("IterRows: %v", err)
			}
			if row["vmaf_model"] != tc.want {
				t.Errorf("vmaf_model = %v, want %q", row["vmaf_model"], tc.want)
			}
		})
	}
}

func TestIterRowsScaleFilterForCrossResolutionRungs(t *testing.T) {
	t.Parallel()

	job, opts := rawYUVJob(t, []Cell{{Preset: "medium", CRF: 26}})
	// A 1280x720 source encoded into a 640x360 rung.
	job.SrcWidth, job.SrcHeight = 1280, 720
	job.Width, job.Height = 640, 360

	var encodeArgv []string
	runners := scriptedRunners(nil)
	base := runners.Encode
	runners.Encode = func(ctx context.Context, argv []string) RunResult {
		encodeArgv = argv
		return base(ctx, argv)
	}
	// The reference must be re-decoded at the rung target, so let the decode
	// succeed and write the sidecar.
	runners.Decode = func(_ context.Context, argv []string) RunResult {
		if err := os.WriteFile(argv[len(argv)-1], []byte("yuv"), 0o600); err != nil {
			return RunResult{ReturnCode: 1}
		}
		return RunResult{}
	}

	if err := IterRows(context.Background(), job, opts, runners,
		func(map[string]any) error { return nil }); err != nil {
		t.Fatalf("IterRows: %v", err)
	}

	joined := strings.Join(encodeArgv, " ")
	if !strings.Contains(joined, "-vf scale=640:360") {
		t.Errorf("encode argv is missing the rung scale filter: %v", encodeArgv)
	}
	if !strings.Contains(joined, "-s 1280x720") {
		t.Errorf("encode argv should declare the source geometry: %v", encodeArgv)
	}
}

func TestBuildRowIsJSONSerialisable(t *testing.T) {
	t.Parallel()

	job, opts := rawYUVJob(t, []Cell{{Preset: "medium", CRF: 26}})
	var row map[string]any
	if err := IterRows(context.Background(), job, opts, scriptedRunners(nil),
		func(r map[string]any) error {
			row = r
			return nil
		}); err != nil {
		t.Fatalf("IterRows: %v", err)
	}

	// The corpus writer renders through the CPython-compatible encoder;
	// every value type in a row must be one it accepts.
	line, err := WriteRowLine(row)
	if err != nil {
		t.Fatalf("WriteRowLine: %v", err)
	}
	// The rendered line must be re-readable, NaN tokens and all.
	path := filepath.Join(t.TempDir(), "corpus.jsonl")
	if wErr := os.WriteFile(path, []byte(line+"\n"), 0o600); wErr != nil {
		t.Fatalf("write line: %v", wErr)
	}
	rows, rErr := ReadJSONL(path, true)
	if rErr != nil {
		t.Fatalf("ReadJSONL: %v", rErr)
	}
	if len(rows) != 1 {
		t.Fatalf("read %d rows, want 1", len(rows))
	}
	if rows[0]["crf"] != 26 {
		t.Errorf("round-tripped crf = %v, want 26", rows[0]["crf"])
	}
}

func TestResolveSampleClip(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name         string
		durationS    float64
		framerate    float64
		clipSeconds  float64
		wantClipMode string
		wantStart    float64
		wantSkipRef  int
		wantFrameCnt int
	}{
		{
			name: "off by default", durationS: 60, framerate: 24, clipSeconds: 0,
			wantClipMode: "full",
		},
		{
			name:      "a centre window of a longer source",
			durationS: 60, framerate: 24, clipSeconds: 10,
			wantClipMode: "sample_10s", wantStart: 25, wantSkipRef: 600, wantFrameCnt: 240,
		},
		{
			name:      "a request at least as long as the source falls back to full",
			durationS: 8, framerate: 24, clipSeconds: 10,
			wantClipMode: "full",
		},
		{
			name:      "a fractional framerate rounds the frame counts",
			durationS: 10, framerate: 23.976, clipSeconds: 4,
			wantClipMode: "sample_4s", wantStart: 3, wantSkipRef: 72, wantFrameCnt: 96,
		},
		{
			name:      "an unknown source duration cannot be sliced",
			durationS: 0, framerate: 24, clipSeconds: 10,
			wantClipMode: "full",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			job := Job{DurationS: tc.durationS, Framerate: tc.framerate}
			opts := Options{SampleClipSeconds: tc.clipSeconds}
			got := resolveSampleClip(job, opts)
			if got.ClipMode != tc.wantClipMode {
				t.Errorf("ClipMode = %q, want %q", got.ClipMode, tc.wantClipMode)
			}
			if tc.wantClipMode == "full" {
				return
			}
			if got.StartS != tc.wantStart {
				t.Errorf("StartS = %v, want %v", got.StartS, tc.wantStart)
			}
			if got.FrameSkipRef != tc.wantSkipRef {
				t.Errorf("FrameSkipRef = %d, want %d", got.FrameSkipRef, tc.wantSkipRef)
			}
			if got.FrameCnt != tc.wantFrameCnt {
				t.Errorf("FrameCnt = %d, want %d", got.FrameCnt, tc.wantFrameCnt)
			}
		})
	}
}

func TestEncodePath(t *testing.T) {
	t.Parallel()

	opts := Options{Encoder: "libx265", EncodeDir: "/scratch/enc"}
	got := encodePath(opts, "/refs/big_buck_bunny.yuv", "slow", 28)
	want := filepath.Join("/scratch/enc", "big_buck_bunny__libx265__slow__crf28.mp4")
	if got != want {
		t.Errorf("encodePath = %q, want %q", got, want)
	}
}

func TestMissingRowKeys(t *testing.T) {
	t.Parallel()

	full := map[string]any{}
	for _, key := range RowKeys {
		full[key] = 0
	}
	if missing := MissingRowKeys(full); len(missing) != 0 {
		t.Errorf("a complete row reported missing keys: %v", missing)
	}
	delete(full, "vmaf_score")
	delete(full, "motion2_std")
	got := MissingRowKeys(full)
	want := []string{"vmaf_score", "motion2_std"}
	if len(got) != len(want) {
		t.Fatalf("MissingRowKeys = %v, want %v", got, want)
	}
	sorted := map[string]bool{}
	for _, k := range got {
		sorted[k] = true
	}
	for _, k := range want {
		if !sorted[k] {
			t.Errorf("MissingRowKeys is missing %q (got %v)", k, got)
		}
	}
}

func TestRowKeysCount(t *testing.T) {
	t.Parallel()

	// 32 base v3 columns + 12 canonical-6 aggregates + 10 encoder-stats,
	// matching len(vmaftune.CORPUS_ROW_KEYS).
	const want = 32 + 12 + 10
	if len(RowKeys) != want {
		t.Errorf("RowKeys has %d entries, want %d", len(RowKeys), want)
	}
	seen := map[string]bool{}
	for _, k := range RowKeys {
		if seen[k] {
			t.Errorf("RowKeys contains %q twice", k)
		}
		seen[k] = true
	}
}

func TestUTCNowISO8601Format(t *testing.T) {
	t.Parallel()

	got := UTCNowISO8601()
	// aiutils.time_utils.now_iso_8601 emits second precision with a numeric
	// UTC offset — never a trailing "Z", which some consumers reject.
	if !strings.HasSuffix(got, "+00:00") {
		t.Errorf("UTCNowISO8601 = %q, want a +00:00 suffix", got)
	}
	if len(got) != len("2026-05-16T12:34:56+00:00") {
		t.Errorf("UTCNowISO8601 = %q, want second precision", got)
	}
}

func TestFileSHA256(t *testing.T) {
	t.Parallel()

	path := filepath.Join(t.TempDir(), "data.bin")
	if err := os.WriteFile(path, []byte("abc"), 0o600); err != nil {
		t.Fatalf("seed file: %v", err)
	}
	got, err := FileSHA256(path)
	if err != nil {
		t.Fatalf("FileSHA256: %v", err)
	}
	// sha256("abc"), the canonical NIST test vector.
	const want = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
	if got != want {
		t.Errorf("FileSHA256 = %q, want %q", got, want)
	}
	if _, err := FileSHA256(filepath.Join(t.TempDir(), "absent")); err == nil {
		t.Error("FileSHA256 on a missing file should error")
	}
}

func TestNewRunIDIsHexAndUnique(t *testing.T) {
	t.Parallel()

	seen := map[string]bool{}
	for i := 0; i < 100; i++ {
		id := NewRunID()
		if len(id) != 32 {
			t.Fatalf("run id %q has length %d, want 32", id, len(id))
		}
		if _, err := strconv.ParseUint(id[:16], 16, 64); err != nil {
			t.Fatalf("run id %q is not hex: %v", id, err)
		}
		if seen[id] {
			t.Fatalf("run id %q was generated twice", id)
		}
		seen[id] = true
	}
}

func TestOptionsDefaults(t *testing.T) {
	t.Parallel()

	opts := NewOptions()
	want := Options{
		Encoder:         "libx264",
		Output:          "corpus.jsonl",
		EncodeDir:       filepath.Join(".workingdir2", "encodes"),
		VMAFModel:       Model1080P,
		FFmpegBin:       "ffmpeg",
		VMAFBin:         "vmaf",
		FFprobeBin:      "ffprobe",
		SrcSHA256:       true,
		HDRMode:         HDRModeAuto,
		ResolutionAware: true,
	}
	if !reflect.DeepEqual(opts, want) {
		t.Errorf("NewOptions() = %+v, want %+v", opts, want)
	}
}

// TestRowIsJSONDecodableByStdlib documents which row values encoding/json can
// carry: the NaN columns cannot, which is exactly why the writer goes through
// pkg/pyjson.
func TestRowIsJSONDecodableByStdlib(t *testing.T) {
	t.Parallel()

	job, opts := rawYUVJob(t, []Cell{{Preset: "medium", CRF: 26}})
	var row map[string]any
	if err := IterRows(context.Background(), job, opts, scriptedRunners(nil),
		func(r map[string]any) error {
			row = r
			return nil
		}); err != nil {
		t.Fatalf("IterRows: %v", err)
	}
	if _, err := json.Marshal(row); err == nil {
		t.Error("encoding/json accepted a row carrying NaN — the pyjson writer " +
			"exists precisely because it should not")
	}
}
