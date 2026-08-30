// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/modeleval/eval.go — native Go implementation of the MCP
// `eval_model_on_split` / `compare_models` tools.
//
// This replaces the embedded python3 script that cmd/vmafx-mcp/impl.go
// used to shell out to (`delegateToPythonEval`), which required a host
// Python carrying numpy + pandas + scipy + onnxruntime. Removing that
// subprocess is the last dependency blocking the Python MCP server
// sunset described in ADR-0704 Stage 2.
//
// Everything except the ONNX forward pass is pure Go and needs no cgo:
// parquet reading (features.go), split bucketing (split.go) and the
// correlation statistics (stats.go). The forward pass is abstracted
// behind the Predictor interface so this package stays testable with a
// stub and CGO_ENABLED=0; the production implementation binds
// libvmaf's own ONNX Runtime session API (pkg/libvmaf.DNNSession),
// which means no third-party ONNX dependency enters the Go module.

package modeleval

import (
	"context"
	"fmt"
	"sort"
)

// Predictor runs one model forward pass.
//
// x is a row-major float32 buffer of rows*cols elements; the
// implementation binds it to the graph input named inputName with shape
// [rows, cols] and returns the output tensor flattened to one value per
// row.
type Predictor interface {
	Predict(ctx context.Context, inputName string, x []float32, rows, cols int) ([]float32, error)
}

// Result is the `eval_model_on_split` response body.
//
// Field order is deliberate: it reproduces the insertion order of the
// dict the Python server returns, so json.Marshal here and json.dumps
// there emit keys in the same sequence.
type Result struct {
	Model    string   `json:"model"`
	Features string   `json:"features"`
	Split    string   `json:"split"`
	N        int      `json:"n"`
	PLCC     float64  `json:"plcc"`
	SROCC    float64  `json:"srocc"`
	RMSE     float64  `json:"rmse"`
	Columns  []string `json:"columns"`
}

// Options selects what to evaluate.
type Options struct {
	// ModelPath is the .onnx file; echoed into Result.Model.
	ModelPath string
	// FeaturesPath is the .parquet feature cache.
	FeaturesPath string
	// Split is one of ValidSplits. Empty means SplitTest.
	Split string
	// InputName is the ONNX graph input to bind. Empty means "features".
	InputName string
}

// DefaultInputName matches the Python server's default argument.
const DefaultInputName = "features"

// Dataset is the model-ready slice of a feature cache: a row-major
// float32 design matrix plus the aligned target vector.
type Dataset struct {
	X       []float32
	Y       []float32
	Rows    int
	Cols    int
	Columns []string
}

// LoadDataset reads featuresPath, filters it to split, and assembles the
// design matrix. It performs every step of the Python original except
// the ONNX call, in the same order and with the same error messages.
func LoadDataset(featuresPath, split string) (*Dataset, error) {
	if err := ValidateSplit(split); err != nil {
		return nil, err
	}

	// The target column has to be requested alongside the features, or
	// LoadTable filters it out and the "no 'mos' column" guard below
	// fires on a table that actually has one.
	want := make([]string, 0, len(FeatureColumns)+1)
	want = append(want, FeatureColumns...)
	want = append(want, TargetColumn)

	tbl, err := LoadTable(featuresPath, want)
	if err != nil {
		return nil, err
	}
	if !tbl.Has(TargetColumn) {
		return nil, fmt.Errorf("%s has no 'mos' column — can't score correlations", featuresPath)
	}

	// Row selection. Python filters only when a key column exists and
	// the caller asked for something narrower than "all"; without a key
	// column every row survives regardless of the requested split.
	keep := make([]int, 0, tbl.Rows)
	if split != SplitAll && tbl.HasKey {
		for i, k := range tbl.Keys {
			if SplitOf(k) == split {
				keep = append(keep, i)
			}
		}
	} else {
		for i := range tbl.Rows {
			keep = append(keep, i)
		}
	}

	cols := make([]string, 0, len(FeatureColumns))
	for _, c := range FeatureColumns {
		if tbl.Has(c) {
			cols = append(cols, c)
		}
	}
	if len(cols) == 0 {
		return nil, fmt.Errorf(
			"%s has none of the expected feature columns "+
				"('adm2', 'vif_scale0', 'vif_scale1', 'vif_scale2', 'vif_scale3', 'motion2'); got %v",
			featuresPath, tbl.Names)
	}

	if len(keep) < 2 {
		return nil, fmt.Errorf("split %q has %d samples — need >=2 to compute correlations",
			split, len(keep))
	}

	target := tbl.Numeric[TargetColumn]
	ds := &Dataset{
		Rows:    len(keep),
		Cols:    len(cols),
		Columns: cols,
		X:       make([]float32, 0, len(keep)*len(cols)),
		Y:       make([]float32, 0, len(keep)),
	}
	for _, r := range keep {
		for _, c := range cols {
			ds.X = append(ds.X, float32(tbl.Numeric[c][r]))
		}
		ds.Y = append(ds.Y, float32(target[r]))
	}
	return ds, nil
}

// Evaluate scores one model against one split and reports PLCC, SROCC
// and RMSE. It is the Go equivalent of `_eval_model_on_split`.
func Evaluate(ctx context.Context, p Predictor, opts Options) (*Result, error) {
	split := opts.Split
	if split == "" {
		split = SplitTest
	}
	inputName := opts.InputName
	if inputName == "" {
		inputName = DefaultInputName
	}

	ds, err := LoadDataset(opts.FeaturesPath, split)
	if err != nil {
		return nil, err
	}

	pred, err := p.Predict(ctx, inputName, ds.X, ds.Rows, ds.Cols)
	if err != nil {
		return nil, err
	}
	if len(pred) != len(ds.Y) {
		return nil, fmt.Errorf("model output shape (%d,) does not match target shape (%d,)",
			len(pred), len(ds.Y))
	}

	pf, yf := toFloat64(pred), toFloat64(ds.Y)
	plcc, err := Pearson(pf, yf)
	if err != nil {
		return nil, fmt.Errorf("plcc: %w", err)
	}
	srocc, err := Spearman(pf, yf)
	if err != nil {
		return nil, fmt.Errorf("srocc: %w", err)
	}
	rmse, err := RMSE(pf, yf)
	if err != nil {
		return nil, fmt.Errorf("rmse: %w", err)
	}

	return &Result{
		Model:    opts.ModelPath,
		Features: opts.FeaturesPath,
		Split:    split,
		N:        ds.Rows,
		PLCC:     plcc,
		SROCC:    srocc,
		RMSE:     rmse,
		Columns:  ds.Columns,
	}, nil
}

// ModelError records one model that could not be evaluated during a
// Compare run.
type ModelError struct {
	Model string `json:"model"`
	Error string `json:"error"`
}

// Comparison is the `compare_models` response body.
type Comparison struct {
	Ranked []*Result    `json:"ranked"`
	Errors []ModelError `json:"errors"`
}

// PredictorFactory opens a Predictor for one model path. Compare uses it
// so each model gets its own session, and so tests can inject stubs.
type PredictorFactory func(modelPath string) (Predictor, func(), error)

// Compare ranks several models on the same split by descending PLCC.
// A model that fails to open or score is collected into Errors rather
// than aborting the run, matching `_compare_models`.
func Compare(ctx context.Context, newPredictor PredictorFactory, models []string, opts Options) *Comparison {
	out := &Comparison{Ranked: []*Result{}, Errors: []ModelError{}}
	for _, m := range models {
		res, err := evalOne(ctx, newPredictor, m, opts)
		if err != nil {
			out.Errors = append(out.Errors, ModelError{Model: m, Error: err.Error()})
			continue
		}
		out.Ranked = append(out.Ranked, res)
	}
	sort.SliceStable(out.Ranked, func(i, j int) bool {
		return out.Ranked[i].PLCC > out.Ranked[j].PLCC
	})
	return out
}

// evalOne opens a predictor for a single model, evaluates it, and always
// releases the session.
func evalOne(ctx context.Context, newPredictor PredictorFactory, model string, opts Options) (*Result, error) {
	p, closeFn, err := newPredictor(model)
	if err != nil {
		return nil, err
	}
	defer closeFn()

	o := opts
	o.ModelPath = model
	return Evaluate(ctx, p, o)
}
