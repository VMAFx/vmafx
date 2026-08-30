// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// Parquet-reader tests. The committed fixture has no "key" column, so
// the split-filtering path is exercised here against parquet files
// written on the fly.

package modeleval

import (
	"math"
	"path/filepath"
	"strings"
	"testing"

	"github.com/parquet-go/parquet-go"
)

// keyedRow mirrors the shape the AI training scripts emit: a string key,
// the six candidate feature columns, and the mos target.
type keyedRow struct {
	Key       string  `parquet:"key,optional,dict"`
	ADM2      float32 `parquet:"adm2,optional"`
	VIFScale0 float32 `parquet:"vif_scale0,optional"`
	VIFScale1 float32 `parquet:"vif_scale1,optional"`
	VIFScale2 float32 `parquet:"vif_scale2,optional"`
	VIFScale3 float32 `parquet:"vif_scale3,optional"`
	Motion2   float32 `parquet:"motion2,optional"`
	MOS       float32 `parquet:"mos,optional"`
	Unrelated int64   `parquet:"row,optional"`
}

// partialRow has only a subset of the feature columns, to prove the
// "whichever are present, in canonical order" rule.
type partialRow struct {
	Motion2 float32 `parquet:"motion2,optional"`
	ADM2    float32 `parquet:"adm2,optional"`
	MOS     float32 `parquet:"mos,optional"`
}

// noTargetRow omits mos entirely.
type noTargetRow struct {
	ADM2 float32 `parquet:"adm2,optional"`
}

// noFeatureRow has a target but none of the feature columns.
type noFeatureRow struct {
	MOS   float32 `parquet:"mos,optional"`
	Other float32 `parquet:"something_else,optional"`
}

func writeParquet[T any](t *testing.T, name string, rows []T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := parquet.WriteFile(path, rows); err != nil {
		t.Fatalf("WriteFile(%s): %v", name, err)
	}
	return path
}

func TestLoadTableReadsCommittedFixture(t *testing.T) {
	tbl, err := LoadTable(fixture, append(append([]string{}, FeatureColumns...), TargetColumn))
	if err != nil {
		t.Fatalf("LoadTable: %v", err)
	}
	if tbl.Rows != 256 {
		t.Errorf("Rows = %d, want 256", tbl.Rows)
	}
	if tbl.HasKey {
		t.Error("fixture unexpectedly has a key column")
	}
	for _, c := range append(append([]string{}, FeatureColumns...), TargetColumn) {
		vals, ok := tbl.Numeric[c]
		if !ok {
			t.Errorf("column %q missing", c)
			continue
		}
		if len(vals) != 256 {
			t.Errorf("column %q has %d values, want 256", c, len(vals))
		}
	}
	// "row" is present in the file but was not requested, so it must not
	// have been materialised.
	if _, ok := tbl.Numeric["row"]; ok {
		t.Error("unrequested column 'row' was materialised")
	}
}

// TestLoadDatasetFiltersByKey is the split path the committed fixture
// cannot reach. Expected membership comes from SplitOf, which split_test
// pins against the Python implementation.
func TestLoadDatasetFiltersByKey(t *testing.T) {
	const n = 400
	rows := make([]keyedRow, n)
	for i := range rows {
		f := float32(i)
		rows[i] = keyedRow{
			Key: keyFor(i), ADM2: f, VIFScale0: f + 1, VIFScale1: f + 2,
			VIFScale2: f + 3, VIFScale3: f + 4, Motion2: f + 5,
			MOS: f * 0.5, Unrelated: int64(i),
		}
	}
	path := writeParquet(t, "keyed.parquet", rows)

	want := map[string]int{}
	for i := range n {
		want[SplitOf(keyFor(i))]++
	}

	total := 0
	for _, split := range []string{SplitTrain, SplitVal, SplitTest} {
		ds, err := LoadDataset(path, split)
		if err != nil {
			t.Fatalf("LoadDataset(%s): %v", split, err)
		}
		if ds.Rows != want[split] {
			t.Errorf("split %q rows = %d, want %d", split, ds.Rows, want[split])
		}
		total += ds.Rows
	}
	if total != n {
		t.Errorf("splits cover %d rows, want %d (rows lost or double-counted)", total, n)
	}

	// "all" must bypass filtering entirely.
	all, err := LoadDataset(path, SplitAll)
	if err != nil {
		t.Fatalf("LoadDataset(all): %v", err)
	}
	if all.Rows != n {
		t.Errorf("split all rows = %d, want %d", all.Rows, n)
	}
}

// TestLoadDatasetKeyRowAlignment proves the filter keeps features and
// target on the same row after selection.
func TestLoadDatasetKeyRowAlignment(t *testing.T) {
	const n = 300
	rows := make([]keyedRow, n)
	for i := range rows {
		// mos = adm2 * 2 for every row, so any misalignment breaks it.
		rows[i] = keyedRow{Key: keyFor(i), ADM2: float32(i), MOS: float32(i) * 2}
	}
	path := writeParquet(t, "aligned.parquet", rows)

	ds, err := LoadDataset(path, SplitTest)
	if err != nil {
		t.Fatalf("LoadDataset: %v", err)
	}
	if ds.Rows == 0 {
		t.Fatal("test split is empty")
	}
	adm2Idx := -1
	for i, c := range ds.Columns {
		if c == "adm2" {
			adm2Idx = i
		}
	}
	if adm2Idx < 0 {
		t.Fatal("adm2 column not selected")
	}
	for r := range ds.Rows {
		got := ds.X[r*ds.Cols+adm2Idx]
		if ds.Y[r] != got*2 {
			t.Fatalf("row %d misaligned: adm2=%v mos=%v (want mos=2*adm2)", r, got, ds.Y[r])
		}
	}
}

func TestLoadDatasetSelectsColumnsInCanonicalOrder(t *testing.T) {
	// Declared motion2-before-adm2 in the file; the loader must still
	// emit them in FeatureColumns order.
	rows := []partialRow{
		{Motion2: 1, ADM2: 2, MOS: 3},
		{Motion2: 4, ADM2: 5, MOS: 6},
		{Motion2: 7, ADM2: 8, MOS: 9},
	}
	path := writeParquet(t, "partial.parquet", rows)

	ds, err := LoadDataset(path, SplitAll)
	if err != nil {
		t.Fatalf("LoadDataset: %v", err)
	}
	want := []string{"adm2", "motion2"}
	if strings.Join(ds.Columns, ",") != strings.Join(want, ",") {
		t.Fatalf("Columns = %v, want %v", ds.Columns, want)
	}
	if ds.Cols != 2 {
		t.Fatalf("Cols = %d, want 2", ds.Cols)
	}
	// Row 0 must be [adm2=2, motion2=1].
	if ds.X[0] != 2 || ds.X[1] != 1 {
		t.Errorf("row 0 = [%v, %v], want [2, 1]", ds.X[0], ds.X[1])
	}
}

func TestLoadDatasetRejections(t *testing.T) {
	twoRowNoTarget := writeParquet(t, "notarget.parquet",
		[]noTargetRow{{ADM2: 1}, {ADM2: 2}})
	noFeatures := writeParquet(t, "nofeat.parquet",
		[]noFeatureRow{{MOS: 1, Other: 9}, {MOS: 2, Other: 8}})
	oneRow := writeParquet(t, "onerow.parquet",
		[]partialRow{{Motion2: 1, ADM2: 2, MOS: 3}})

	cases := []struct {
		name      string
		path      string
		split     string
		wantSubst string
	}{
		{"missing mos column", twoRowNoTarget, SplitAll, "has no 'mos' column"},
		{"no feature columns", noFeatures, SplitAll, "none of the expected feature columns"},
		{"fewer than two rows", oneRow, SplitAll, "need >=2 to compute correlations"},
		{"bad split name", fixture, "nope", "split must be one of"},
		{"unreadable file", filepath.Join(t.TempDir(), "nope.parquet"), SplitAll, "open parquet"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := LoadDataset(tc.path, tc.split)
			if err == nil {
				t.Fatal("expected an error")
			}
			if !strings.Contains(err.Error(), tc.wantSubst) {
				t.Errorf("error = %q, want it to contain %q", err, tc.wantSubst)
			}
		})
	}
}

// TestLoadTableNotParquet guards the "file exists but is not parquet"
// path, which a mistyped --features argument hits routinely.
func TestLoadTableNotParquet(t *testing.T) {
	path := filepath.Join(t.TempDir(), "junk.parquet")
	if err := writeBytes(path, []byte("this is not parquet")); err != nil {
		t.Fatalf("write: %v", err)
	}
	if _, err := LoadTable(path, FeatureColumns); err == nil {
		t.Fatal("expected an error for a non-parquet file")
	}
}

func TestTableHas(t *testing.T) {
	tbl := &Table{Numeric: map[string][]float64{"adm2": {1}}, HasKey: true}
	cases := []struct {
		name string
		col  string
		want bool
	}{
		{"present numeric", "adm2", true},
		{"absent numeric", "motion2", false},
		{"key when present", KeyColumn, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := tbl.Has(tc.col); got != tc.want {
				t.Errorf("Has(%q) = %v, want %v", tc.col, got, tc.want)
			}
		})
	}
	noKey := &Table{Numeric: map[string][]float64{}, HasKey: false}
	if noKey.Has(KeyColumn) {
		t.Error("Has(key) = true on a table without a key column")
	}
}

// TestValueConversionHelpers pins the null and type mapping that keeps
// the reader aligned with pandas' behaviour.
func TestValueConversionHelpers(t *testing.T) {
	if got := valueToFloat(parquet.ValueOf(float32(1.5))); got != 1.5 {
		t.Errorf("float32 -> %v, want 1.5", got)
	}
	if got := valueToFloat(parquet.ValueOf(float64(2.25))); got != 2.25 {
		t.Errorf("float64 -> %v, want 2.25", got)
	}
	if got := valueToFloat(parquet.ValueOf(int64(7))); got != 7 {
		t.Errorf("int64 -> %v, want 7", got)
	}
	if got := valueToFloat(parquet.ValueOf(true)); got != 1 {
		t.Errorf("bool true -> %v, want 1", got)
	}
	if got := valueToFloat(parquet.Value{}); !math.IsNaN(got) {
		t.Errorf("null -> %v, want NaN", got)
	}
	if got := valueToString(parquet.ValueOf("clip_1")); got != "clip_1" {
		t.Errorf("string -> %q, want clip_1", got)
	}
	if got := valueToString(parquet.ValueOf(int64(42))); got != "42" {
		t.Errorf("int64 -> %q, want \"42\"", got)
	}
	if got := valueToString(parquet.Value{}); got != "" {
		t.Errorf("null -> %q, want empty", got)
	}
}
