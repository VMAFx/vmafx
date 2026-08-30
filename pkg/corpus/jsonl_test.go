// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/jsonl_test.go — corpus JSONL reader / writer tests.
//
// The corpus JSONL is a cross-implementation contract: Python writes it today
// and the Phase B/C trainers read it. The writer therefore has to emit the
// bare NaN tokens CPython's json module produces, and the reader has to accept
// them coming back.

package corpus

import (
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestWriteRowLine(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		row  map[string]any
		want string
	}{
		{
			name: "keys are sorted and integers stay integral",
			row: map[string]any{
				"crf": 26, "preset": "medium", "schema_version": SchemaVersion,
			},
			want: `{"crf": 26, "preset": "medium", "schema_version": 3}`,
		},
		{
			name: "a missing canonical-6 feature renders as the bare NaN token",
			row:  map[string]any{"adm2_mean": math.NaN(), "vmaf_score": 93.5},
			want: `{"adm2_mean": NaN, "vmaf_score": 93.5}`,
		},
		{
			name: "an integral float keeps its .0",
			row:  map[string]any{"framerate": 24.0, "duration_s": 10.0},
			want: `{"duration_s": 10.0, "framerate": 24.0}`,
		},
		{
			name: "extra_params renders as a JSON array",
			row:  map[string]any{"extra_params": []string{"-vf", "scale=640:480"}},
			want: `{"extra_params": ["-vf", "scale=640:480"]}`,
		},
		{
			name: "hdr_forced renders as a JSON boolean",
			row:  map[string]any{"hdr_forced": true},
			want: `{"hdr_forced": true}`,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := WriteRowLine(tc.row)
			if err != nil {
				t.Fatalf("WriteRowLine: %v", err)
			}
			if got != tc.want {
				t.Errorf("WriteRowLine() =\n  %s\nwant\n  %s", got, tc.want)
			}
		})
	}
}

func TestWriteAndReadJSONL(t *testing.T) {
	t.Parallel()

	path := filepath.Join(t.TempDir(), "nested", "corpus.jsonl")
	rows := []map[string]any{
		{
			"schema_version": SchemaVersion, "crf": 26, "preset": "medium",
			"vmaf_score": 96.5, "adm2_mean": 0.98, "adm2_std": math.NaN(),
			"framerate": 24.0, "hdr_forced": false,
			"extra_params": []string{"-vf", "scale=640:480"},
		},
		{
			"schema_version": SchemaVersion, "crf": 32, "preset": "medium",
			"vmaf_score": math.NaN(), "adm2_mean": math.NaN(), "adm2_std": math.NaN(),
			"framerate": 24.0, "hdr_forced": true,
			"extra_params": []string{},
		},
	}

	n, err := WriteJSONL(rows, path)
	if err != nil {
		t.Fatalf("WriteJSONL: %v", err)
	}
	if n != 2 {
		t.Errorf("WriteJSONL wrote %d rows, want 2", n)
	}

	raw, readErr := os.ReadFile(path) //nolint:gosec // test-owned temp path
	if readErr != nil {
		t.Fatalf("read jsonl: %v", readErr)
	}
	if !strings.Contains(string(raw), "NaN") {
		t.Error("the JSONL is missing the bare NaN tokens the Python writer emits")
	}
	if lines := strings.Count(string(raw), "\n"); lines != 2 {
		t.Errorf("the JSONL has %d newlines, want one per row", lines)
	}

	// upgrade=false keeps the on-disk row verbatim; upgrade=true would
	// backfill the missing canonical-6 columns with NaN (see
	// TestReadJSONLUpgradesLegacyRows) and change the re-written bytes.
	back, readJSONLErr := ReadJSONL(path, false)
	if readJSONLErr != nil {
		t.Fatalf("ReadJSONL: %v", readJSONLErr)
	}
	if len(back) != 2 {
		t.Fatalf("ReadJSONL returned %d rows, want 2", len(back))
	}
	if back[0]["crf"] != 26 {
		t.Errorf("round-tripped crf = %v (%T), want the int 26", back[0]["crf"], back[0]["crf"])
	}
	if back[0]["vmaf_score"] != 96.5 {
		t.Errorf("round-tripped vmaf_score = %v, want 96.5", back[0]["vmaf_score"])
	}
	if v, _ := back[1]["vmaf_score"].(float64); !math.IsNaN(v) {
		t.Errorf("round-tripped NaN score = %v, want NaN", v)
	}
	if back[1]["hdr_forced"] != true {
		t.Errorf("round-tripped hdr_forced = %v, want true", back[1]["hdr_forced"])
	}

	// The round trip must be byte-stable: re-writing what was read produces
	// the same file.
	rewritten := filepath.Join(t.TempDir(), "again.jsonl")
	if _, wErr := WriteJSONL(back, rewritten); wErr != nil {
		t.Fatalf("re-write: %v", wErr)
	}
	raw2, readErr2 := os.ReadFile(rewritten) //nolint:gosec // test-owned temp path
	if readErr2 != nil {
		t.Fatalf("read re-written jsonl: %v", readErr2)
	}
	if string(raw) != string(raw2) {
		t.Errorf("the write -> read -> write round trip is not byte-stable:\n%s\nvs\n%s",
			raw, raw2)
	}
}

func TestReadJSONLTolerance(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		body    string
		wantN   int
		wantErr bool
	}{
		{
			name:  "blank lines are skipped",
			body:  "{\"crf\": 26}\n\n   \n{\"crf\": 32}\n",
			wantN: 2,
		},
		{
			name:  "the bare Infinity tokens decode too",
			body:  "{\"a\": Infinity, \"b\": -Infinity, \"c\": NaN}\n",
			wantN: 1,
		},
		{
			// A string that merely spells NaN must survive as a string.
			name:  "a NaN inside a string literal is untouched",
			body:  "{\"stderr_tail\": \"score was NaN\"}\n",
			wantN: 1,
		},
		{name: "a malformed line errors", body: "{oops\n", wantErr: true},
		{name: "a non-object line errors", body: "[1, 2, 3]\n", wantErr: true},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			path := filepath.Join(t.TempDir(), "corpus.jsonl")
			if err := os.WriteFile(path, []byte(tc.body), 0o600); err != nil {
				t.Fatalf("seed jsonl: %v", err)
			}
			rows, err := ReadJSONL(path, true)
			if (err != nil) != tc.wantErr {
				t.Fatalf("ReadJSONL error = %v, wantErr = %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if len(rows) != tc.wantN {
				t.Fatalf("ReadJSONL returned %d rows, want %d", len(rows), tc.wantN)
			}
			if tc.name == "the bare Infinity tokens decode too" {
				if v, _ := rows[0]["a"].(float64); !math.IsInf(v, 1) {
					t.Errorf("a = %v, want +Inf", rows[0]["a"])
				}
				if v, _ := rows[0]["b"].(float64); !math.IsInf(v, -1) {
					t.Errorf("b = %v, want -Inf", rows[0]["b"])
				}
				if v, _ := rows[0]["c"].(float64); !math.IsNaN(v) {
					t.Errorf("c = %v, want NaN", rows[0]["c"])
				}
			}
			if tc.name == "a NaN inside a string literal is untouched" {
				if rows[0]["stderr_tail"] != "score was NaN" {
					t.Errorf("stderr_tail = %v, want the original string",
						rows[0]["stderr_tail"])
				}
			}
		})
	}
}

func TestReadJSONLUpgradesLegacyRows(t *testing.T) {
	t.Parallel()

	path := filepath.Join(t.TempDir(), "v2.jsonl")
	// A schema-v2 row predates the canonical-6 columns (ADR-0366).
	body := `{"schema_version": 2, "crf": 26, "vmaf_score": 93.5}` + "\n"
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatalf("seed jsonl: %v", err)
	}

	upgraded, err := ReadJSONL(path, true)
	if err != nil {
		t.Fatalf("ReadJSONL: %v", err)
	}
	for _, key := range append(append([]string{}, Canonical6MeanKeys...), Canonical6StdKeys...) {
		v, ok := upgraded[0][key]
		if !ok {
			t.Fatalf("the upgrade did not backfill %q", key)
		}
		if f, isFloat := v.(float64); !isFloat || !math.IsNaN(f) {
			t.Errorf("%s = %v, want NaN", key, v)
		}
	}
	// schema_version itself is preserved so callers can still filter < 3.
	if upgraded[0]["schema_version"] != 2 {
		t.Errorf("schema_version = %v, want the original 2", upgraded[0]["schema_version"])
	}

	bare, err := ReadJSONL(path, false)
	if err != nil {
		t.Fatalf("ReadJSONL(upgrade=false): %v", err)
	}
	if _, ok := bare[0]["adm2_mean"]; ok {
		t.Error("ReadJSONL(upgrade=false) backfilled the v3 columns anyway")
	}
}

func TestUpgradeRowIsDefensiveOnCurrentRows(t *testing.T) {
	t.Parallel()

	// A partially-written current-schema row is backfilled too, so
	// positional consumers do not crash on a truncated column.
	row := map[string]any{"schema_version": SchemaVersion, "adm2_mean": 0.98}
	UpgradeRow(row)
	if row["adm2_mean"] != 0.98 {
		t.Errorf("the upgrade overwrote a real value: %v", row["adm2_mean"])
	}
	if v, ok := row["motion2_std"].(float64); !ok || !math.IsNaN(v) {
		t.Errorf("motion2_std = %v, want the NaN backfill", row["motion2_std"])
	}
}

func TestReadJSONLMissingFile(t *testing.T) {
	t.Parallel()

	if _, err := ReadJSONL(filepath.Join(t.TempDir(), "absent.jsonl"), true); err == nil {
		t.Error("ReadJSONL on a missing file should error")
	}
}
