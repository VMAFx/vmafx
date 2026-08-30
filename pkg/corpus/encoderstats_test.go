// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/encoderstats_test.go — pass-1 stats parser and aggregator tests.
//
// The expected aggregates were produced by feeding the same lines through
// vmaftune.encoder_stats.parse_stats_line + aggregate_stats. They are compared
// bit-exactly rather than with a tolerance: the values land in the corpus JSONL
// verbatim, so a ULP of drift is a visible byte difference.

package corpus

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

// x264 and x265 sample lines, verbatim from the encoder_stats module docstring.
const (
	x264StatsLine = "in:0 out:0 type:I dur:2 cpbdur:2 q:25.23 aq:20.39 tex:14051 " +
		"mv:1126 misc:5871 imb:80 pmb:0 smb:0 d:- ref:;"
	x265StatsLine = "in:1 out:1 type:P q:27.83 q-aq:28.41 q-noVbv:27.83 tex:1317 " +
		"mv:200 misc:191 icu:0.53 pcu:12.00 scu:67.47 sc:0 ;"
)

func TestParseStatsLine(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name   string
		line   string
		wantOK bool
		want   PerFrameStats
	}{
		{
			name: "x264 I-frame line", line: x264StatsLine, wantOK: true,
			want: PerFrameStats{
				InIdx: 0, OutIdx: 0, FrameType: "I", QP: 25.23, AQ: 20.39,
				Tex: 14051, MV: 1126, Misc: 5871, IMB: 80, PMB: 0, SMB: 0,
			},
		},
		{
			// x265 spells aq as q-aq and the CTU counts as icu / pcu / scu,
			// and emits them as fractions.
			name: "x265 P-frame line with the field aliases",
			line: x265StatsLine, wantOK: true,
			want: PerFrameStats{
				InIdx: 1, OutIdx: 1, FrameType: "P", QP: 27.83, AQ: 28.41,
				Tex: 1317, MV: 200, Misc: 191, IMB: 0.53, PMB: 12.0, SMB: 67.47,
			},
		},
		{name: "the options header is skipped", line: "#options: whatever"},
		{name: "a blank line is skipped", line: "   "},
		{name: "a line without in/type is skipped", line: "garbage line"},
		{name: "a line with in but no type is skipped", line: "in:5 q:20"},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, ok := ParseStatsLine(tc.line)
			if ok != tc.wantOK {
				t.Fatalf("ParseStatsLine ok = %v, want %v", ok, tc.wantOK)
			}
			if !ok {
				return
			}
			if got != tc.want {
				t.Errorf("ParseStatsLine() = %+v, want %+v", got, tc.want)
			}
		})
	}
}

func TestPerFrameStatsDerivedFields(t *testing.T) {
	t.Parallel()

	f, ok := ParseStatsLine(x264StatsLine)
	if !ok {
		t.Fatal("fixture line did not parse")
	}
	if got := f.Bits(); got != 14051+1126+5871 {
		t.Errorf("Bits() = %d, want tex+mv+misc", got)
	}
	if got := f.MBTotal(); got != 80 {
		t.Errorf("MBTotal() = %v, want 80", got)
	}
	if !f.IsIntra() {
		t.Error("an I frame should report IsIntra")
	}

	for _, tc := range []struct {
		frameType string
		want      bool
	}{
		{"I", true}, {"i", true}, {"P", false}, {"B", false}, {"b", false},
	} {
		got := PerFrameStats{FrameType: tc.frameType}.IsIntra()
		if got != tc.want {
			t.Errorf("IsIntra(%q) = %v, want %v", tc.frameType, got, tc.want)
		}
	}
}

func TestAggregateStats(t *testing.T) {
	t.Parallel()

	i, _ := ParseStatsLine(x264StatsLine)
	p, _ := ParseStatsLine(x265StatsLine)

	tests := []struct {
		name   string
		frames []PerFrameStats
		want   map[string]float64
	}{
		{
			name:   "empty input yields the all-zero schema",
			frames: nil,
			want: map[string]float64{
				"enc_internal_qp_mean": 0, "enc_internal_qp_std": 0,
				"enc_internal_bits_mean": 0, "enc_internal_bits_std": 0,
				"enc_internal_mv_mean": 0, "enc_internal_mv_std": 0,
				"enc_internal_itex_mean": 0, "enc_internal_ptex_mean": 0,
				"enc_internal_intra_ratio": 0, "enc_internal_skip_ratio": 0,
			},
		},
		{
			name:   "a single intra frame",
			frames: []PerFrameStats{i},
			want: map[string]float64{
				"enc_internal_qp_mean": 25.23, "enc_internal_qp_std": 0,
				"enc_internal_bits_mean": 21048.0, "enc_internal_bits_std": 0,
				"enc_internal_mv_mean": 1126.0, "enc_internal_mv_std": 0,
				"enc_internal_itex_mean": 14051.0, "enc_internal_ptex_mean": 0,
				"enc_internal_intra_ratio": 1.0, "enc_internal_skip_ratio": 0,
			},
		},
		{
			name:   "an intra plus an inter frame",
			frames: []PerFrameStats{i, p},
			want: map[string]float64{
				"enc_internal_qp_mean": 26.53, "enc_internal_qp_std": 1.299999999999999,
				"enc_internal_bits_mean": 11378.0, "enc_internal_bits_std": 9670.0,
				"enc_internal_mv_mean": 663.0, "enc_internal_mv_std": 463.0,
				"enc_internal_itex_mean": 14051.0, "enc_internal_ptex_mean": 1317.0,
				"enc_internal_intra_ratio": 0.5033125, "enc_internal_skip_ratio": 0.4216875,
			},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := AggregateStats(tc.frames)
			if !reflect.DeepEqual(got, tc.want) {
				for _, key := range EncoderStatsColumns {
					if got[key] != tc.want[key] {
						t.Errorf("%s = %v, want %v", key, got[key], tc.want[key])
					}
				}
			}
			if len(got) != len(EncoderStatsColumns) {
				t.Errorf("aggregate emitted %d columns, want %d",
					len(got), len(EncoderStatsColumns))
			}
		})
	}
}

func TestParseStatsFile(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	path := filepath.Join(dir, "stats-0.log")
	body := "#options: crf=23\n" + x264StatsLine + "\n\n" + x265StatsLine + "\ntrailing garbage\n"
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatalf("seed stats file: %v", err)
	}

	frames := ParseStatsFile(path)
	if len(frames) != 2 {
		t.Fatalf("parsed %d frames, want 2", len(frames))
	}
	if frames[0].FrameType != "I" || frames[1].FrameType != "P" {
		t.Errorf("frame types = %q / %q, want I / P", frames[0].FrameType, frames[1].FrameType)
	}

	// A missing file is not an error — the row records zero aggregates.
	if got := ParseStatsFile(filepath.Join(dir, "absent-0.log")); got != nil {
		t.Errorf("ParseStatsFile on a missing file = %v, want nil", got)
	}
}

func TestParseStatsFileToleratesTruncation(t *testing.T) {
	t.Parallel()

	// An encoder killed mid-write leaves a partial final line; the parser
	// must keep the frames it already read rather than failing the cell.
	path := filepath.Join(t.TempDir(), "stats-0.log")
	body := x264StatsLine + "\nin:1 out:1 type:P q:not-a-number tex:\n"
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatalf("seed stats file: %v", err)
	}
	frames := ParseStatsFile(path)
	if len(frames) != 2 {
		t.Fatalf("parsed %d frames, want 2 (the malformed one coerces to zeros)", len(frames))
	}
	if frames[1].QP != 0 || frames[1].Tex != 0 {
		t.Errorf("malformed tokens = (qp %v, tex %d), want zeros",
			frames[1].QP, frames[1].Tex)
	}
}
