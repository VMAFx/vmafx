// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package recommend_test

import (
	"encoding/json"
	"math"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/recommend"
	"github.com/VMAFx/vmafx/pkg/uncertainty"
)

// row is a terse constructor for a corpus row.
func row(crf int, vmaf, kbps float64) recommend.Row {
	return recommend.Row{
		"encoder":      "libx264",
		"preset":       "medium",
		"src":          "/src/a.yuv",
		"crf":          float64(crf),
		"vmaf_score":   vmaf,
		"bitrate_kbps": kbps,
		"exit_status":  float64(0),
	}
}

// withInterval attaches a conformal interval block to a row.
func withInterval(r recommend.Row, low, high float64) recommend.Row {
	r["vmaf_interval"] = map[string]any{"low": low, "high": high}
	return r
}

// f returns a pointer to v, for the optional Request targets.
func f(v float64) *float64 { return &v }

// TestValidateRequest pins the mutually-exclusive target contract.
func TestValidateRequest(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		req     recommend.Request
		wantErr bool
	}{
		{"target vmaf only", recommend.Request{TargetVMAF: f(93)}, false},
		{"target bitrate only", recommend.Request{TargetBitrateKbps: f(5000)}, false},
		{
			"both targets", recommend.Request{
				TargetVMAF: f(93), TargetBitrateKbps: f(5000),
			}, true,
		},
		{"no target", recommend.Request{}, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if err := recommend.ValidateRequest(tc.req); (err != nil) != tc.wantErr {
				t.Errorf("ValidateRequest error = %v, wantErr %v", err, tc.wantErr)
			}
		})
	}
}

// TestPickTargetVMAF covers the smallest-passing-CRF rule, the determinism
// tie-break, and the UNMET fallback.
func TestPickTargetVMAF(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name          string
		rows          []recommend.Row
		target        float64
		wantCRF       int
		wantPredicate string
		wantMargin    float64
		wantErr       bool
	}{
		{
			name: "smallest passing CRF wins",
			rows: []recommend.Row{
				row(20, 96.0, 8000), row(24, 93.5, 5000), row(28, 90.0, 3000),
			},
			target: 93.0, wantCRF: 20,
			wantPredicate: "target_vmaf>=93.0", wantMargin: 3.0,
		},
		{
			name: "row order does not matter",
			rows: []recommend.Row{
				row(28, 90.0, 3000), row(24, 93.5, 5000), row(20, 96.0, 8000),
			},
			target: 93.0, wantCRF: 20,
			wantPredicate: "target_vmaf>=93.0", wantMargin: 3.0,
		},
		{
			name: "duplicate CRF ties break to the higher score",
			rows: []recommend.Row{
				row(24, 93.2, 5000), row(24, 94.9, 5200),
			},
			target: 93.0, wantCRF: 24,
			wantPredicate: "target_vmaf>=93.0", wantMargin: 1.9,
		},
		{
			name: "nothing clears the bar returns the closest miss",
			rows: []recommend.Row{
				row(28, 90.0, 3000), row(30, 88.0, 2500),
			},
			target: 95.0, wantCRF: 28,
			wantPredicate: "target_vmaf>=95.0 (UNMET)", wantMargin: -5.0,
		},
		{
			name:   "fractional target keeps its decimal",
			rows:   []recommend.Row{row(24, 94.0, 5000)},
			target: 93.5, wantCRF: 24,
			wantPredicate: "target_vmaf>=93.5", wantMargin: 0.5,
		},
		{
			name: "empty row set is an error",
			rows: nil, target: 93.0, wantErr: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := recommend.PickTargetVMAF(tc.rows, tc.target)
			if (err != nil) != tc.wantErr {
				t.Fatalf("PickTargetVMAF error = %v, wantErr %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if crf := int(got.Row["crf"].(float64)); crf != tc.wantCRF {
				t.Errorf("winning crf = %d, want %d", crf, tc.wantCRF)
			}
			if got.Predicate != tc.wantPredicate {
				t.Errorf("predicate = %q, want %q", got.Predicate, tc.wantPredicate)
			}
			if math.Abs(got.Margin-tc.wantMargin) > 1e-9 {
				t.Errorf("margin = %v, want %v", got.Margin, tc.wantMargin)
			}
		})
	}
}

// TestPickTargetBitrate covers the closest-distance rule and the lower-CRF
// tie-break.
func TestPickTargetBitrate(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name          string
		rows          []recommend.Row
		target        float64
		wantCRF       int
		wantPredicate string
		wantMargin    float64
		wantErr       bool
	}{
		{
			name: "closest bitrate wins",
			rows: []recommend.Row{
				row(20, 96.0, 8000), row(24, 93.5, 5200), row(28, 90.0, 3000),
			},
			target: 5000, wantCRF: 24,
			wantPredicate: "|bitrate-5000.0|->min", wantMargin: 200,
		},
		{
			name: "equidistant ties go to the lower CRF",
			rows: []recommend.Row{
				row(28, 90.0, 4500), row(20, 96.0, 5500),
			},
			target: 5000, wantCRF: 20,
			wantPredicate: "|bitrate-5000.0|->min", wantMargin: 500,
		},
		{
			name:   "under-target pick reports a negative margin",
			rows:   []recommend.Row{row(30, 88.0, 2500)},
			target: 5000, wantCRF: 30,
			wantPredicate: "|bitrate-5000.0|->min", wantMargin: -2500,
		},
		{
			name: "empty row set is an error",
			rows: nil, target: 5000, wantErr: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := recommend.PickTargetBitrate(tc.rows, tc.target)
			if (err != nil) != tc.wantErr {
				t.Fatalf("PickTargetBitrate error = %v, wantErr %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if crf := int(got.Row["crf"].(float64)); crf != tc.wantCRF {
				t.Errorf("winning crf = %d, want %d", crf, tc.wantCRF)
			}
			if got.Predicate != tc.wantPredicate {
				t.Errorf("predicate = %q, want %q", got.Predicate, tc.wantPredicate)
			}
			if math.Abs(got.Margin-tc.wantMargin) > 1e-9 {
				t.Errorf("margin = %v, want %v", got.Margin, tc.wantMargin)
			}
		})
	}
}

// TestRecommend_filtering asserts the eligibility filter drops the rows the
// Python _filter_rows drops: wrong encoder, wrong preset, non-zero exit
// status, missing or non-finite score.
func TestRecommend_filtering(t *testing.T) {
	t.Parallel()

	failed := row(18, 99.0, 12000)
	failed["exit_status"] = float64(1)

	otherEncoder := row(18, 98.0, 11000)
	otherEncoder["encoder"] = "libx265"

	otherPreset := row(18, 97.5, 10500)
	otherPreset["preset"] = "slow"

	noScore := row(18, 0, 10000)
	delete(noScore, "vmaf_score")

	nanScore := row(18, 0, 10000)
	nanScore["vmaf_score"] = math.NaN()

	rows := []recommend.Row{
		failed, otherEncoder, otherPreset, noScore, nanScore,
		row(24, 93.5, 5000),
	}

	got, err := recommend.Recommend(rows, recommend.Request{
		TargetVMAF: f(93.0), Encoder: "libx264", Preset: "medium",
	})
	if err != nil {
		t.Fatalf("Recommend: %v", err)
	}
	if crf := int(got.Row["crf"].(float64)); crf != 24 {
		t.Errorf("winning crf = %d, want 24 (every other row should be filtered)", crf)
	}
}

// TestRecommend_allRowsFiltered asserts that filtering everything out is an
// error rather than a silent bad pick.
func TestRecommend_allRowsFiltered(t *testing.T) {
	t.Parallel()

	_, err := recommend.Recommend(
		[]recommend.Row{row(24, 93.5, 5000)},
		recommend.Request{TargetVMAF: f(93.0), Encoder: "libsvtav1"},
	)
	if err == nil {
		t.Fatal("expected an error when the filter removes every row")
	}
	if !strings.Contains(err.Error(), "no eligible rows") {
		t.Errorf("error = %q, want it to mention no eligible rows", err)
	}
}

// TestPickTargetVMAFWithUncertainty covers each of the four decision paths.
func TestPickTargetVMAFWithUncertainty(t *testing.T) {
	t.Parallel()

	thresholds := uncertainty.Thresholds{
		TightIntervalMaxWidth: 2.0, WideIntervalMinWidth: 5.0,
	}

	tests := []struct {
		name         string
		rows         []recommend.Row
		target       float64
		wantCRF      int
		wantDecision uncertainty.Decision
		wantVisited  int
		wantContains string
	}{
		{
			name: "tight interval short-circuits at the first clearing row",
			rows: []recommend.Row{
				withInterval(row(20, 96.0, 8000), 95.0, 96.5),
				withInterval(row(24, 93.5, 5000), 93.0, 94.0),
				withInterval(row(28, 90.0, 3000), 89.5, 90.5),
			},
			target: 93.0, wantCRF: 20,
			wantDecision: uncertainty.Tight, wantVisited: 1,
			wantContains: "(TIGHT, low=95.000)",
		},
		{
			name: "wide intervals force the full scan and tag UNCERTAIN",
			rows: []recommend.Row{
				withInterval(row(20, 96.0, 8000), 90.0, 99.0),
				withInterval(row(24, 93.5, 5000), 88.0, 99.0),
			},
			target: 93.0, wantCRF: 20,
			wantDecision: uncertainty.Wide, wantVisited: 2,
			wantContains: "(UNCERTAIN)",
		},
		{
			name: "uncalibrated rows defer to the point estimate",
			rows: []recommend.Row{
				row(20, 96.0, 8000), row(24, 93.5, 5000),
			},
			target: 93.0, wantCRF: 20,
			wantDecision: uncertainty.Middle, wantVisited: 2,
			wantContains: "target_vmaf>=93.0",
		},
		{
			name: "every interval below the target reports interval-excluded",
			rows: []recommend.Row{
				withInterval(row(28, 90.0, 3000), 89.5, 90.5),
				withInterval(row(30, 88.0, 2500), 87.5, 88.5),
			},
			target: 95.0, wantCRF: 28,
			wantDecision: uncertainty.Middle, wantVisited: 2,
			wantContains: "(UNMET, interval-excluded)",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := recommend.PickTargetVMAFWithUncertainty(tc.rows,
				recommend.UncertaintyRequest{
					TargetVMAF: tc.target, Thresholds: thresholds,
				})
			if err != nil {
				t.Fatalf("PickTargetVMAFWithUncertainty: %v", err)
			}
			if crf := int(got.Row["crf"].(float64)); crf != tc.wantCRF {
				t.Errorf("winning crf = %d, want %d", crf, tc.wantCRF)
			}
			if got.Decision != tc.wantDecision {
				t.Errorf("decision = %q, want %q", got.Decision, tc.wantDecision)
			}
			if got.Visited != tc.wantVisited {
				t.Errorf("visited = %d, want %d", got.Visited, tc.wantVisited)
			}
			if !strings.Contains(got.Predicate, tc.wantContains) {
				t.Errorf("predicate = %q, want it to contain %q",
					got.Predicate, tc.wantContains)
			}
		})
	}
}

// TestPickTargetVMAFWithUncertainty_zeroWidthIsNotTight is the regression
// guard for the NaN-preservation rule: a row with NO interval must classify
// MIDDLE, not as a zero-width TIGHT that short-circuits on a "lower bound"
// which is really just the point estimate.
func TestPickTargetVMAFWithUncertainty_zeroWidthIsNotTight(t *testing.T) {
	t.Parallel()

	rows := []recommend.Row{
		// No interval; score clears the target. A spurious TIGHT
		// short-circuit here would pick CRF 24 and stop at Visited=1.
		row(24, 93.5, 5000),
		row(20, 96.0, 8000),
	}
	got, err := recommend.PickTargetVMAFWithUncertainty(rows,
		recommend.UncertaintyRequest{
			TargetVMAF: 93.0, Thresholds: uncertainty.DefaultThresholds(),
		})
	if err != nil {
		t.Fatalf("PickTargetVMAFWithUncertainty: %v", err)
	}
	if got.Decision != uncertainty.Middle {
		t.Errorf("decision = %q, want %q", got.Decision, uncertainty.Middle)
	}
	if got.Visited != 2 {
		t.Errorf("visited = %d, want 2 (no short-circuit without an interval)", got.Visited)
	}
	if crf := int(got.Row["crf"].(float64)); crf != 20 {
		t.Errorf("winning crf = %d, want 20 (the point-estimate pick)", crf)
	}
}

// TestPickTargetVMAFWithUncertainty_sampleOverride covers the out-of-band
// interval injection seam.
func TestPickTargetVMAFWithUncertainty_sampleOverride(t *testing.T) {
	t.Parallel()

	rows := []recommend.Row{
		// The embedded interval is wide; the override makes it tight.
		withInterval(row(20, 96.0, 8000), 80.0, 99.0),
	}
	got, err := recommend.PickTargetVMAFWithUncertainty(rows,
		recommend.UncertaintyRequest{
			TargetVMAF: 93.0,
			Thresholds: uncertainty.DefaultThresholds(),
			SampleUncertainty: map[int]recommend.Interval{
				20: {Low: 95.0, High: 96.5},
			},
		})
	if err != nil {
		t.Fatalf("PickTargetVMAFWithUncertainty: %v", err)
	}
	if got.Decision != uncertainty.Tight {
		t.Errorf("decision = %q, want %q (the override should win)",
			got.Decision, uncertainty.Tight)
	}
}

// TestParseCorpusJSONL covers blank-line tolerance and the malformed-line
// rejection (dropping a row would bias the recommendation).
func TestParseCorpusJSONL(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name     string
		input    string
		wantRows int
		wantErr  bool
	}{
		{
			name: "well-formed stream",
			input: `{"crf":20,"vmaf_score":96.0}` + "\n" +
				`{"crf":24,"vmaf_score":93.5}` + "\n",
			wantRows: 2,
		},
		{
			name: "blank lines are skipped",
			input: "\n" + `{"crf":20,"vmaf_score":96.0}` + "\n\n  \n" +
				`{"crf":24,"vmaf_score":93.5}` + "\n",
			wantRows: 2,
		},
		{
			name:     "empty stream",
			input:    "",
			wantRows: 0,
		},
		{
			name:    "malformed line is an error",
			input:   `{"crf":20}` + "\n" + `{truncated` + "\n",
			wantErr: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			rows, err := recommend.ParseCorpusJSONL(strings.NewReader(tc.input))
			if (err != nil) != tc.wantErr {
				t.Fatalf("ParseCorpusJSONL error = %v, wantErr %v", err, tc.wantErr)
			}
			if !tc.wantErr && len(rows) != tc.wantRows {
				t.Errorf("row count = %d, want %d", len(rows), tc.wantRows)
			}
		})
	}
}

// TestSanitizeNonFiniteTokens covers the Python-token normalisation, and in
// particular that a string value containing the literal text "NaN" (a source
// path, a model name) is left alone.
func TestSanitizeNonFiniteTokens(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		in   string
		want string
	}{
		{
			name: "no tokens is a pass-through",
			in:   `{"crf": 20, "vmaf_score": 96.0}`,
			want: `{"crf": 20, "vmaf_score": 96.0}`,
		},
		{
			name: "bare NaN becomes null",
			in:   `{"adm2_mean": NaN, "crf": 20}`,
			want: `{"adm2_mean": null, "crf": 20}`,
		},
		{
			name: "both infinities",
			in:   `{"hi": Infinity, "lo": -Infinity}`,
			want: `{"hi": null, "lo": null}`,
		},
		{
			name: "a path containing NaN survives",
			in:   `{"src": "/corpus/NaN_clips/a.yuv", "adm2_mean": NaN}`,
			want: `{"src": "/corpus/NaN_clips/a.yuv", "adm2_mean": null}`,
		},
		{
			name: "an escaped quote does not end the string early",
			in:   `{"src": "/a\"NaN\"b.yuv", "adm2_mean": NaN}`,
			want: `{"src": "/a\"NaN\"b.yuv", "adm2_mean": null}`,
		},
		{
			name: "Infinity inside a string survives",
			in:   `{"vmaf_model": "path=/m/Infinity.json", "x": Infinity}`,
			want: `{"vmaf_model": "path=/m/Infinity.json", "x": null}`,
		},
		{
			name: "-Infinity is matched before Infinity",
			in:   `{"lo": -Infinity}`,
			want: `{"lo": null}`,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got := string(recommend.SanitizeNonFiniteTokens([]byte(tc.in)))
			if got != tc.want {
				t.Errorf("SanitizeNonFiniteTokens =\n %s\nwant\n %s", got, tc.want)
			}
			// Whatever comes out must be parseable, which is the whole point.
			var probe map[string]any
			if err := json.Unmarshal([]byte(got), &probe); err != nil {
				t.Errorf("sanitised line is not valid JSON: %v", err)
			}
		})
	}
}

// TestParseCorpusJSONL_pythonNonFiniteTokens is the regression guard for the
// bug a live round trip caught: the Go reader rejected the bare NaN tokens the
// Python writer (and this port's own writer) emit for unmeasured aggregates.
func TestParseCorpusJSONL_pythonNonFiniteTokens(t *testing.T) {
	t.Parallel()

	// A row as Python's json.dumps writes it, unmeasured aggregates and all.
	line := `{"adm2_mean": NaN, "adm2_std": NaN, "bitrate_kbps": 93.0, ` +
		`"crf": 10, "encoder": "libx264", "exit_status": 0, ` +
		`"preset": "veryfast", "src": "ref.yuv", "vmaf_score": 97.568}`

	rows, err := recommend.ParseCorpusJSONL(strings.NewReader(line + "\n"))
	if err != nil {
		t.Fatalf("ParseCorpusJSONL: %v", err)
	}
	if len(rows) != 1 {
		t.Fatalf("row count = %d, want 1", len(rows))
	}
	// The row must still be pickable: the NaN aggregate is irrelevant to the
	// predicate, and the finite vmaf_score must survive.
	got, pickErr := recommend.PickTargetVMAF(rows, 93.0)
	if pickErr != nil {
		t.Fatalf("PickTargetVMAF: %v", pickErr)
	}
	if crf := int(got.Row["crf"].(float64)); crf != 10 {
		t.Errorf("winning crf = %d, want 10", crf)
	}
	// The unmeasured aggregate reads as absent, not as a synthetic zero.
	if v := got.Row["adm2_mean"]; v != nil {
		t.Errorf("adm2_mean = %v, want nil for an unmeasured aggregate", v)
	}
}

// TestParseCorpusJSONL_nanScoreRowIsFiltered asserts a row whose SCORE is NaN
// is dropped by the eligibility filter, exactly as a math.NaN() score is.
func TestParseCorpusJSONL_nanScoreRowIsFiltered(t *testing.T) {
	t.Parallel()

	input := `{"crf": 20, "vmaf_score": NaN, "exit_status": 0}` + "\n" +
		`{"crf": 24, "vmaf_score": 93.5, "exit_status": 0}` + "\n"

	rows, err := recommend.ParseCorpusJSONL(strings.NewReader(input))
	if err != nil {
		t.Fatalf("ParseCorpusJSONL: %v", err)
	}
	got, pickErr := recommend.Recommend(rows, recommend.Request{TargetVMAF: f(93.0)})
	if pickErr != nil {
		t.Fatalf("Recommend: %v", pickErr)
	}
	if crf := int(got.Row["crf"].(float64)); crf != 24 {
		t.Errorf("winning crf = %d, want 24 (the NaN-scored row must be filtered)", crf)
	}
}

// TestSmallestPassingCRF covers the (src, preset)-grouped picker the
// encode-driven path uses.
func TestSmallestPassingCRF(t *testing.T) {
	t.Parallel()

	mk := func(src, preset string, crf int, vmaf float64) recommend.Row {
		return recommend.Row{
			"src": src, "preset": preset,
			"crf": float64(crf), "vmaf_score": vmaf,
		}
	}

	tests := []struct {
		name      string
		rows      []recommend.Row
		target    float64
		wantOK    bool
		wantSrc   string
		wantCRF   int
		wantScore float64
	}{
		{
			name: "smallest passing CRF per group",
			rows: []recommend.Row{
				mk("/a.yuv", "medium", 28, 90.0),
				mk("/a.yuv", "medium", 24, 93.5),
				mk("/a.yuv", "medium", 20, 96.0),
			},
			target: 93.0, wantOK: true,
			wantSrc: "/a.yuv", wantCRF: 20, wantScore: 96.0,
		},
		{
			name: "first group in row order wins",
			rows: []recommend.Row{
				mk("/b.yuv", "fast", 22, 94.0),
				mk("/a.yuv", "medium", 20, 96.0),
			},
			target: 93.0, wantOK: true,
			wantSrc: "/b.yuv", wantCRF: 22, wantScore: 94.0,
		},
		{
			name: "nothing clears the target",
			rows: []recommend.Row{
				mk("/a.yuv", "medium", 28, 90.0),
			},
			target: 95.0, wantOK: false,
		},
		{
			name: "empty input", rows: nil, target: 93.0, wantOK: false,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			src, _, crf, score, ok := recommend.SmallestPassingCRF(tc.rows, tc.target)
			if ok != tc.wantOK {
				t.Fatalf("ok = %v, want %v", ok, tc.wantOK)
			}
			if !ok {
				return
			}
			if src != tc.wantSrc || crf != tc.wantCRF || score != tc.wantScore {
				t.Errorf("got (%q, %d, %v), want (%q, %d, %v)",
					src, crf, score, tc.wantSrc, tc.wantCRF, tc.wantScore)
			}
		})
	}
}

// TestResultRow_roundTripsJSON asserts the winning row still marshals to the
// original record, which is what the --json output emits.
func TestResultRow_roundTripsJSON(t *testing.T) {
	t.Parallel()

	src := `{"crf":24,"vmaf_score":93.5,"bitrate_kbps":5000,"custom_key":"kept"}`
	rows, err := recommend.ParseCorpusJSONL(strings.NewReader(src))
	if err != nil {
		t.Fatalf("ParseCorpusJSONL: %v", err)
	}
	got, pickErr := recommend.PickTargetVMAF(rows, 93.0)
	if pickErr != nil {
		t.Fatalf("PickTargetVMAF: %v", pickErr)
	}
	out, marshalErr := json.Marshal(got.Row)
	if marshalErr != nil {
		t.Fatalf("marshal winning row: %v", marshalErr)
	}
	if !strings.Contains(string(out), `"custom_key":"kept"`) {
		t.Errorf("unknown keys must survive into the --json output; got %s", out)
	}
}
