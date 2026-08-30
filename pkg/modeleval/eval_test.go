// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// End-to-end tests for the native eval path, exercised against the
// committed pandas/pyarrow fixture ai/testdata/bisect/features.parquet
// (256 rows, zstd, optional FLOAT columns).
//
// The forward pass is stubbed with a fixed linear model so the whole
// pipeline — parquet read, column selection, split filter, statistics —
// is verified without needing an ONNX Runtime build. The expected
// PLCC / SROCC / RMSE were computed by scipy over the identical design
// matrix and identical weights; see the golden note in stats_test.go.

package modeleval

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"path/filepath"
	"strings"
	"testing"
)

// fixture is the committed feature cache, relative to this package.
const fixture = "../../ai/testdata/bisect/features.parquet"

// linearStub is a deterministic stand-in for an ONNX regressor: a fixed
// dot product per row. Weights are exact binary fractions so the Go and
// Python evaluations of the same expression agree bit-for-bit.
type linearStub struct {
	weights   []float64
	gotInput  string
	gotRows   int
	gotCols   int
	callCount int
}

var stubWeights = []float64{0.5, -0.25, 0.125, 0.0625, -0.03125, 0.75}

func (s *linearStub) Predict(_ context.Context, inputName string, x []float32, rows, cols int) ([]float32, error) {
	s.gotInput, s.gotRows, s.gotCols = inputName, rows, cols
	s.callCount++
	out := make([]float32, rows)
	for r := range rows {
		var acc float64
		for c := range cols {
			acc += float64(x[r*cols+c]) * s.weights[c]
		}
		out[r] = float32(acc)
	}
	return out, nil
}

// errStub always fails, to exercise the Compare error-collection path.
type errStub struct{ msg string }

func (e *errStub) Predict(context.Context, string, []float32, int, int) ([]float32, error) {
	return nil, errors.New(e.msg)
}

// badShapeStub returns the wrong number of predictions.
type badShapeStub struct{ n int }

func (b *badShapeStub) Predict(context.Context, string, []float32, int, int) ([]float32, error) {
	return make([]float32, b.n), nil
}

func TestLoadDatasetFromFixture(t *testing.T) {
	ds, err := LoadDataset(fixture, SplitAll)
	if err != nil {
		t.Fatalf("LoadDataset: %v", err)
	}
	if ds.Rows != 256 {
		t.Errorf("Rows = %d, want 256", ds.Rows)
	}
	if ds.Cols != 6 {
		t.Errorf("Cols = %d, want 6", ds.Cols)
	}
	wantCols := []string{"adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2"}
	if strings.Join(ds.Columns, ",") != strings.Join(wantCols, ",") {
		t.Errorf("Columns = %v, want %v", ds.Columns, wantCols)
	}
	if len(ds.X) != ds.Rows*ds.Cols {
		t.Errorf("len(X) = %d, want %d", len(ds.X), ds.Rows*ds.Cols)
	}
	if len(ds.Y) != ds.Rows {
		t.Errorf("len(Y) = %d, want %d", len(ds.Y), ds.Rows)
	}
	// Row-major layout: the first row must be the first value of each
	// feature column in canonical order.
	wantRow0 := []float32{0.610475, 0.67854184, 0.70349157, 0.82865155, 0.28198817, 0.7099821}
	for i, w := range wantRow0 {
		if ds.X[i] != w {
			t.Errorf("X[0][%d] = %v, want %v", i, ds.X[i], w)
		}
	}
	if ds.Y[0] != 0.6341402 {
		t.Errorf("Y[0] = %v, want 0.6341402", ds.Y[0])
	}
	for i, v := range ds.X {
		if math.IsNaN(float64(v)) {
			t.Fatalf("X[%d] is NaN; fixture has no nulls", i)
		}
	}
}

// TestEvaluateAgainstScipyGoldens is the end-to-end parity check.
func TestEvaluateAgainstScipyGoldens(t *testing.T) {
	// scipy over the identical design matrix and identical float32
	// predictions (verified elementwise-equal between the two languages
	// before the goldens were taken).
	const (
		wantPLCC  = 0.5414686170686327
		wantSROCC = 0.54029454680704947
		wantRMSE  = 0.1852282700107554
	)
	stub := &linearStub{weights: stubWeights}
	res, err := Evaluate(context.Background(), stub, Options{
		ModelPath:    "/models/stub.onnx",
		FeaturesPath: fixture,
		Split:        SplitAll,
	})
	if err != nil {
		t.Fatalf("Evaluate: %v", err)
	}
	if math.Abs(res.PLCC-wantPLCC) > tol {
		t.Errorf("PLCC = %.17g, want %.17g", res.PLCC, wantPLCC)
	}
	if math.Abs(res.SROCC-wantSROCC) > tol {
		t.Errorf("SROCC = %.17g, want %.17g", res.SROCC, wantSROCC)
	}
	if math.Abs(res.RMSE-wantRMSE) > tol {
		t.Errorf("RMSE = %.17g, want %.17g", res.RMSE, wantRMSE)
	}
	if res.N != 256 {
		t.Errorf("N = %d, want 256", res.N)
	}
	if res.Model != "/models/stub.onnx" || res.Features != fixture || res.Split != SplitAll {
		t.Errorf("echoed fields wrong: %+v", res)
	}
	// Defaults must reach the predictor.
	if stub.gotInput != DefaultInputName {
		t.Errorf("input name = %q, want %q", stub.gotInput, DefaultInputName)
	}
	if stub.gotRows != 256 || stub.gotCols != 6 {
		t.Errorf("tensor shape = [%d, %d], want [256, 6]", stub.gotRows, stub.gotCols)
	}
}

// TestResultJSONKeyOrder pins the response schema. Key order matters:
// it reproduces the insertion order of the dict the Python server
// returns, so both servers emit identical JSON for the same result.
func TestResultJSONKeyOrder(t *testing.T) {
	b, err := json.Marshal(&Result{
		Model: "m.onnx", Features: "f.parquet", Split: "test", N: 3,
		PLCC: 0.5, SROCC: 0.25, RMSE: 1.5, Columns: []string{"adm2"},
	})
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	want := `{"model":"m.onnx","features":"f.parquet","split":"test","n":3,` +
		`"plcc":0.5,"srocc":0.25,"rmse":1.5,"columns":["adm2"]}`
	if string(b) != want {
		t.Errorf("JSON =\n  %s\nwant\n  %s", b, want)
	}
}

func TestEvaluateDefaultsToTestSplit(t *testing.T) {
	// The fixture has no key column, so every split keeps all 256 rows
	// — exactly the Python behaviour, which only filters when a "key"
	// column exists.
	res, err := Evaluate(context.Background(), &linearStub{weights: stubWeights}, Options{
		FeaturesPath: fixture,
	})
	if err != nil {
		t.Fatalf("Evaluate: %v", err)
	}
	if res.Split != SplitTest {
		t.Errorf("Split = %q, want %q", res.Split, SplitTest)
	}
	if res.N != 256 {
		t.Errorf("N = %d, want 256 (no key column means no filtering)", res.N)
	}
}

func TestEvaluateErrors(t *testing.T) {
	cases := []struct {
		name      string
		opts      Options
		pred      Predictor
		wantSubst string
	}{
		{
			name:      "invalid split",
			opts:      Options{FeaturesPath: fixture, Split: "nope"},
			pred:      &linearStub{weights: stubWeights},
			wantSubst: "split must be one of",
		},
		{
			name:      "missing file",
			opts:      Options{FeaturesPath: filepath.Join(t.TempDir(), "absent.parquet"), Split: SplitAll},
			pred:      &linearStub{weights: stubWeights},
			wantSubst: "open parquet",
		},
		{
			name:      "predictor failure propagates",
			opts:      Options{FeaturesPath: fixture, Split: SplitAll},
			pred:      &errStub{msg: "ort exploded"},
			wantSubst: "ort exploded",
		},
		{
			name:      "output shape mismatch",
			opts:      Options{FeaturesPath: fixture, Split: SplitAll},
			pred:      &badShapeStub{n: 10},
			wantSubst: "does not match target shape",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := Evaluate(context.Background(), tc.pred, tc.opts)
			if err == nil {
				t.Fatalf("expected an error")
			}
			if !strings.Contains(err.Error(), tc.wantSubst) {
				t.Errorf("error = %q, want it to contain %q", err, tc.wantSubst)
			}
		})
	}
}

// TestEvaluateDegenerateOutput covers the constant-prediction case,
// where scipy would emit NaN and json.Marshal would then fail.
func TestEvaluateDegenerateOutput(t *testing.T) {
	zero := &linearStub{weights: []float64{0, 0, 0, 0, 0, 0}}
	_, err := Evaluate(context.Background(), zero, Options{FeaturesPath: fixture, Split: SplitAll})
	if err == nil {
		t.Fatal("expected a degenerate-input error for a constant prediction")
	}
	if !errors.Is(err, ErrDegenerate) {
		t.Errorf("error = %v, want ErrDegenerate", err)
	}
}

func TestCompareRanksByDescendingPLCC(t *testing.T) {
	// Three stubs with different weight vectors produce three different
	// PLCCs; Compare must order them high-to-low and keep failures out
	// of the ranking.
	factories := map[string]Predictor{
		"good.onnx": &linearStub{weights: stubWeights},
		"flipped.onnx": &linearStub{weights: []float64{
			-0.5, 0.25, -0.125, -0.0625, 0.03125, -0.75,
		}},
		"broken.onnx": &errStub{msg: "cannot load"},
	}
	newPredictor := func(path string) (Predictor, func(), error) {
		p, ok := factories[path]
		if !ok {
			return nil, nil, errors.New("unknown model")
		}
		return p, func() {}, nil
	}

	cmp := Compare(context.Background(), newPredictor,
		[]string{"broken.onnx", "flipped.onnx", "good.onnx", "missing.onnx"},
		Options{FeaturesPath: fixture, Split: SplitAll})

	if len(cmp.Ranked) != 2 {
		t.Fatalf("len(Ranked) = %d, want 2 (%+v)", len(cmp.Ranked), cmp.Ranked)
	}
	if cmp.Ranked[0].PLCC < cmp.Ranked[1].PLCC {
		t.Errorf("not sorted descending: %v then %v", cmp.Ranked[0].PLCC, cmp.Ranked[1].PLCC)
	}
	if cmp.Ranked[0].Model != "good.onnx" {
		t.Errorf("best model = %q, want good.onnx", cmp.Ranked[0].Model)
	}
	if len(cmp.Errors) != 2 {
		t.Fatalf("len(Errors) = %d, want 2 (%+v)", len(cmp.Errors), cmp.Errors)
	}
	for _, e := range cmp.Errors {
		if e.Model != "broken.onnx" && e.Model != "missing.onnx" {
			t.Errorf("unexpected error entry %+v", e)
		}
	}
}

// TestCompareEmptyMarshalsAsArrays keeps the JSON shape stable: the
// Python server always emits lists, never null.
func TestCompareEmptyMarshalsAsArrays(t *testing.T) {
	cmp := Compare(context.Background(),
		func(string) (Predictor, func(), error) { return nil, nil, errors.New("x") },
		nil, Options{FeaturesPath: fixture})
	b, err := json.Marshal(cmp)
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	if string(b) != `{"ranked":[],"errors":[]}` {
		t.Errorf("JSON = %s, want {\"ranked\":[],\"errors\":[]}", b)
	}
}

// TestCompareClosesEverySession guards against session leaks — the
// Python original leaked ORT sessions (see docs/research/0983).
func TestCompareClosesEverySession(t *testing.T) {
	closed := 0
	newPredictor := func(string) (Predictor, func(), error) {
		return &linearStub{weights: stubWeights}, func() { closed++ }, nil
	}
	models := []string{"a.onnx", "b.onnx", "c.onnx"}
	Compare(context.Background(), newPredictor, models, Options{FeaturesPath: fixture, Split: SplitAll})
	if closed != len(models) {
		t.Errorf("closed %d sessions, want %d", closed, len(models))
	}
}
