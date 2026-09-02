// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/benchmark/benchmark_test.go — parity tests for the Go port of
// vmaftune.benchmark.
//
// testdata/corpus.jsonl is a hand-built Phase-A corpus that exercises the
// awkward paths: encoders that clear the target and encoders that never do,
// bitrate ties that force the first-wins tie-break, a failed row (exit_status
// 1), a null vmaf_score, an empty encoder token, an exit_status given as the
// string "0", rows missing duration/score timings, an absent crf and a null
// crf.
//
// The golden files beside it were produced by running the *Python*
// vmaftune.benchmark against that same corpus:
//
//	summaries = summarize_benchmark(load_corpus_jsonl(corpus),
//	                                target_vmaf=T, baseline_encoder=B)
//	render_benchmark(summaries, fmt=F)
//
// so every assertion here is a byte-for-byte comparison against the
// implementation this port replaces, not against the port's own output.

package benchmark

import (
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// readGolden loads one golden file verbatim (no newline normalisation — the
// CSV goldens carry CRLF line endings on purpose).
func readGolden(t *testing.T, name string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("testdata", name))
	if err != nil {
		t.Fatalf("read golden %s: %v", name, err)
	}
	return string(b)
}

// loadCorpus loads the shared corpus fixture.
func loadCorpus(t *testing.T) []Row {
	t.Helper()
	rows, err := LoadCorpusJSONL(filepath.Join("testdata", "corpus.jsonl"))
	if err != nil {
		t.Fatalf("load corpus: %v", err)
	}
	return rows
}

// TestRenderMatchesPython is the headline parity gate: four target/baseline
// scenarios x three output formats, each compared to the bytes CPython
// produced.
func TestRenderMatchesPython(t *testing.T) {
	t.Parallel()

	rows := loadCorpus(t)

	scenarios := []struct {
		name     string
		target   float64
		baseline string
		golden   string
	}{
		// Mixed: four encoders clear 92, two do not.
		{"target 92, auto baseline", 92.0, "", "t92_autobase"},
		// Same corpus, baseline pinned to an encoder that is not the
		// lowest-bitrate winner.
		{"target 92, baseline libx265", 92.0, "libx265", "t92_base_x265"},
		// Nothing clears: every encoder reports its closest miss and no
		// baseline exists, so every delta is null / blank.
		{"target 99, nothing clears", 99.0, "", "t99_all_unmet"},
		// Low target: almost everything clears, reshuffling the ranking.
		{"target 85, auto baseline", 85.0, "", "t85_autobase"},
	}

	formats := []struct{ format, ext string }{
		{"json", "json"},
		{"csv", "csv"},
		{"markdown", "md"},
	}

	for _, sc := range scenarios {
		t.Run(sc.name, func(t *testing.T) {
			t.Parallel()
			summaries, err := Summarize(rows, sc.target, sc.baseline)
			if err != nil {
				t.Fatalf("Summarize: %v", err)
			}
			for _, f := range formats {
				got, err := Render(summaries, f.format)
				if err != nil {
					t.Fatalf("Render(%s): %v", f.format, err)
				}
				want := readGolden(t, sc.golden+"."+f.ext)
				if got != want {
					t.Errorf("%s output differs from CPython\n--- got ---\n%q\n--- want ---\n%q",
						f.format, got, want)
				}
			}
		})
	}
}

// TestRenderCSVUsesCRLF pins the line terminator explicitly. Python's csv
// module defaults to the "excel" dialect (CRLF); Go's encoding/csv defaults to
// LF, so this is a one-character difference that would corrupt every line of
// the output.
func TestRenderCSVUsesCRLF(t *testing.T) {
	t.Parallel()

	rows := loadCorpus(t)
	summaries, err := Summarize(rows, 92.0, "")
	if err != nil {
		t.Fatalf("Summarize: %v", err)
	}
	out, err := RenderCSV(summaries)
	if err != nil {
		t.Fatalf("RenderCSV: %v", err)
	}
	if !strings.Contains(out, "\r\n") {
		t.Error("CSV output has no CRLF line ending")
	}
	if strings.Contains(strings.ReplaceAll(out, "\r\n", ""), "\n") {
		t.Error("CSV output contains a bare LF line ending")
	}
}

func TestSummarizeRanking(t *testing.T) {
	t.Parallel()

	rows := loadCorpus(t)
	summaries, err := Summarize(rows, 92.0, "")
	if err != nil {
		t.Fatalf("Summarize: %v", err)
	}

	// Cleared encoders sort first by ascending bitrate; unmet encoders trail,
	// also by ascending bitrate. libvvenc and libx265 tie at 2500 kbps, so the
	// encoder name breaks the tie.
	wantOrder := []string{
		"libvvenc", "libx265", "libx264", "h264_nvenc", // ok
		"libvpx-vp9", "libsvtav1", // unmet
	}
	if len(summaries) != len(wantOrder) {
		t.Fatalf("got %d summaries, want %d", len(summaries), len(wantOrder))
	}
	for i, want := range wantOrder {
		if summaries[i].Encoder != want {
			t.Errorf("summaries[%d].Encoder = %q, want %q", i, summaries[i].Encoder, want)
		}
	}

	byName := map[string]Summary{}
	for _, s := range summaries {
		byName[s.Encoder] = s
	}

	// libx264 has three eligible rows across two sources and two presets; the
	// failed row (exit_status 1) is excluded.
	x264 := byName["libx264"]
	if x264.Rows != 3 || x264.SourceCount != 2 || x264.PresetCount != 2 {
		t.Errorf("libx264 rows/sources/presets = %d/%d/%d, want 3/2/2",
			x264.Rows, x264.SourceCount, x264.PresetCount)
	}
	// The reported point is the cheapest row still clearing 92, not the
	// highest-scoring one.
	if x264.Status != "ok" || x264.BitratekBps != 3100.5 {
		t.Errorf("libx264 = %s @ %v kbps, want ok @ 3100.5", x264.Status, x264.BitratekBps)
	}

	// libsvtav1 never clears 92, so it reports its closest miss.
	svt := byName["libsvtav1"]
	if svt.Status != "unmet" {
		t.Errorf("libsvtav1 status = %q, want unmet", svt.Status)
	}
	if svt.Margin >= 0 {
		t.Errorf("libsvtav1 margin = %v, want negative", svt.Margin)
	}
	// The closest miss is the higher-scoring row (91.9), not the cheaper one.
	if vmaf, ok := finiteFloat(svt.BestRow["vmaf_score"]); !ok || math.Abs(vmaf-91.9) > 1e-9 {
		t.Errorf("libsvtav1 best row vmaf = %v, want 91.9", svt.BestRow["vmaf_score"])
	}

	// An encoder with no timing fields produces no fps means.
	vp9 := byName["libvpx-vp9"]
	if vp9.ScoreFPS != nil {
		t.Errorf("libvpx-vp9 ScoreFPS = %v, want nil (no score_time_ms)", *vp9.ScoreFPS)
	}
	if vp9.EncodeFPS == nil {
		t.Error("libvpx-vp9 EncodeFPS = nil, want a mean over its one timed row")
	}
}

func TestSummarizeBaseline(t *testing.T) {
	t.Parallel()

	rows := loadCorpus(t)

	t.Run("auto baseline is the cheapest cleared encoder", func(t *testing.T) {
		t.Parallel()
		summaries, err := Summarize(rows, 92.0, "")
		if err != nil {
			t.Fatalf("Summarize: %v", err)
		}
		if summaries[0].BitrateDeltaPct == nil || *summaries[0].BitrateDeltaPct != 0 {
			t.Errorf("cheapest encoder delta = %v, want 0", summaries[0].BitrateDeltaPct)
		}
	})

	t.Run("nothing clears leaves every delta nil", func(t *testing.T) {
		t.Parallel()
		summaries, err := Summarize(rows, 99.0, "")
		if err != nil {
			t.Fatalf("Summarize: %v", err)
		}
		for _, s := range summaries {
			if s.Status != "unmet" {
				t.Errorf("%s status = %q, want unmet", s.Encoder, s.Status)
			}
			if s.BitrateDeltaPct != nil {
				t.Errorf("%s delta = %v, want nil", s.Encoder, *s.BitrateDeltaPct)
			}
		}
	})

	t.Run("explicit baseline is honoured", func(t *testing.T) {
		t.Parallel()
		summaries, err := Summarize(rows, 92.0, "libx264")
		if err != nil {
			t.Fatalf("Summarize: %v", err)
		}
		for _, s := range summaries {
			if s.Encoder != "libx264" {
				continue
			}
			if s.BitrateDeltaPct == nil || *s.BitrateDeltaPct != 0 {
				t.Errorf("baseline delta = %v, want 0", s.BitrateDeltaPct)
			}
		}
	})

	t.Run("absent baseline is an error", func(t *testing.T) {
		t.Parallel()
		if _, err := Summarize(rows, 92.0, "libtheora"); err == nil {
			t.Fatal("Summarize accepted a baseline encoder absent from the corpus")
		}
	})
}

func TestSummarizeRejectsEmptyCorpus(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		rows    []Row
		wantErr error
	}{
		{"no rows at all", nil, ErrNoEligibleRows},
		{
			name:    "every row failed",
			rows:    []Row{{"encoder": "libx264", "exit_status": json.Number("1")}},
			wantErr: ErrNoEligibleRows,
		},
		{
			name: "no finite vmaf",
			rows: []Row{{
				"encoder": "libx264", "exit_status": json.Number("0"),
				"vmaf_score": nil, "bitrate_kbps": json.Number("100"),
			}},
			wantErr: ErrNoEligibleRows,
		},
		{
			name: "eligible rows carry no encoder name",
			rows: []Row{{
				"encoder": "", "exit_status": json.Number("0"),
				"vmaf_score": json.Number("95"), "bitrate_kbps": json.Number("100"),
			}},
			wantErr: ErrNoEncoderNames,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			_, err := Summarize(tt.rows, 92.0, "")
			if err != tt.wantErr {
				t.Errorf("Summarize err = %v, want %v", err, tt.wantErr)
			}
		})
	}
}

func TestRenderRejectsUnknownFormat(t *testing.T) {
	t.Parallel()

	if _, err := Render(nil, "pdf"); err == nil {
		t.Fatal("Render accepted an unknown format")
	}
}

func TestParseCorpusJSONL(t *testing.T) {
	t.Parallel()

	t.Run("blank lines are skipped", func(t *testing.T) {
		t.Parallel()
		rows, err := ParseCorpusJSONL(strings.NewReader(
			"{\"a\": 1}\n\n   \n{\"b\": 2}\n"))
		if err != nil {
			t.Fatalf("ParseCorpusJSONL: %v", err)
		}
		if len(rows) != 2 {
			t.Fatalf("got %d rows, want 2", len(rows))
		}
	})

	t.Run("integer literals keep their identity", func(t *testing.T) {
		t.Parallel()
		rows, err := ParseCorpusJSONL(strings.NewReader(`{"crf": 23, "bitrate": 23.0}`))
		if err != nil {
			t.Fatalf("ParseCorpusJSONL: %v", err)
		}
		if got := rows[0]["crf"].(json.Number).String(); got != "23" {
			t.Errorf("crf literal = %q, want \"23\"", got)
		}
		if got := rows[0]["bitrate"].(json.Number).String(); got != "23.0" {
			t.Errorf("bitrate literal = %q, want \"23.0\"", got)
		}
	})

	t.Run("malformed line is an error naming the line number", func(t *testing.T) {
		t.Parallel()
		_, err := ParseCorpusJSONL(strings.NewReader("{\"a\": 1}\nnot json\n"))
		if err == nil {
			t.Fatal("ParseCorpusJSONL accepted a malformed line")
		}
		if !strings.Contains(err.Error(), "line 2") {
			t.Errorf("error %q does not name the offending line", err)
		}
	})
}

// TestCoercionMatchesCPython pins the float()/int() semantics the eligibility
// filter depends on. Getting these wrong silently drops or admits corpus rows.
func TestCoercionMatchesCPython(t *testing.T) {
	t.Parallel()

	floatTests := []struct {
		name string
		in   any
		want float64
		ok   bool
	}{
		{"int literal", json.Number("23"), 23, true},
		{"float literal", json.Number("23.5"), 23.5, true},
		{"numeric string", "23.5", 23.5, true},
		{"whitespace-padded string", "  23.5  ", 23.5, true},
		{"bool true is one", true, 1, true},
		{"nan string is not finite", "nan", 0, false},
		{"inf literal is not finite", "inf", 0, false},
		{"null is not a float", nil, 0, false},
		{"non-numeric string", "medium", 0, false},
		{"list is not a float", []any{}, 0, false},
	}
	for _, tt := range floatTests {
		t.Run("float/"+tt.name, func(t *testing.T) {
			t.Parallel()
			got, ok := finiteFloat(tt.in)
			if ok != tt.ok || (ok && got != tt.want) {
				t.Errorf("finiteFloat(%v) = (%v, %v), want (%v, %v)",
					tt.in, got, ok, tt.want, tt.ok)
			}
		})
	}

	intTests := []struct {
		name string
		in   any
		want int
		ok   bool
	}{
		{"int literal", json.Number("0"), 0, true},
		{"integer string", "0", 0, true},
		// CPython's int("0.0") raises ValueError — the row is skipped.
		{"float string is rejected", "0.0", 0, false},
		// int(0.0) truncates fine, though.
		{"float literal truncates", json.Number("0.9"), 0, true},
		{"negative float truncates toward zero", json.Number("-1.9"), -1, true},
		{"null is not an int", nil, 0, false},
	}
	for _, tt := range intTests {
		t.Run("int/"+tt.name, func(t *testing.T) {
			t.Parallel()
			got, ok := toInt(tt.in)
			if ok != tt.ok || (ok && got != tt.want) {
				t.Errorf("toInt(%v) = (%v, %v), want (%v, %v)",
					tt.in, got, ok, tt.want, tt.ok)
			}
		})
	}
}
