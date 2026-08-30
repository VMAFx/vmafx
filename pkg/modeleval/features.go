// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/modeleval/features.go — parquet feature-cache reader.
//
// Replaces the `pd.read_parquet(features)` call in the Python
// `_eval_model_on_split` with a pure-Go reader (github.com/parquet-go/parquet-go,
// no cgo, no Arrow). Verified against the committed pandas/pyarrow
// fixture ai/testdata/bisect/features.parquet (zstd-compressed, 256 rows,
// optional FLOAT columns) which the AI training scripts produce via
// `df.to_parquet(engine="pyarrow", compression="zstd")`.
//
// Only the columns the evaluator actually needs are materialised, so a
// wide feature table costs no more than a narrow one.

package modeleval

import (
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"strconv"

	"github.com/parquet-go/parquet-go"
)

// FeatureColumns is the candidate input-feature set, in the canonical
// order the model's input tensor expects. Mirrors `_FEATURE_COLUMNS` in
// mcp-server/vmaf-mcp/src/vmaf_mcp/server.py. Columns absent from the
// table are skipped; the surviving order is preserved.
var FeatureColumns = []string{
	"adm2",
	"vif_scale0",
	"vif_scale1",
	"vif_scale2",
	"vif_scale3",
	"motion2",
}

// TargetColumn is the ground-truth subjective score column.
const TargetColumn = "mos"

// KeyColumn drives deterministic split bucketing when present.
const KeyColumn = "key"

// Table is a column-oriented view of the subset of a parquet feature
// cache that the evaluator reads.
type Table struct {
	// Rows is the total row count read from the file.
	Rows int
	// Numeric maps column name to its values widened to float64, with
	// SQL NULL represented as NaN (matching pandas, which surfaces a
	// null as NaN once the column is cast to a float dtype).
	Numeric map[string][]float64
	// Keys holds the stringified KeyColumn when the table has one.
	Keys []string
	// HasKey reports whether KeyColumn was present.
	HasKey bool
	// Names lists every leaf column in file order.
	Names []string
}

// Has reports whether the table carries the named column.
func (t *Table) Has(name string) bool {
	if name == KeyColumn {
		return t.HasKey
	}
	_, ok := t.Numeric[name]
	return ok
}

// LoadTable reads the requested columns from the parquet file at path.
//
// want lists the numeric columns to materialise; missing ones are simply
// absent from the result rather than an error, so the caller can apply
// the "whichever of these are present" rule. The key column is always
// read when the file has one.
func LoadTable(path string, want []string) (*Table, error) {
	f, err := os.Open(path) // #nosec G304 -- path is validated by the caller (MCP ValidatePath).
	if err != nil {
		return nil, fmt.Errorf("open parquet: %w", err)
	}
	defer func() { _ = f.Close() }()

	info, err := f.Stat()
	if err != nil {
		return nil, fmt.Errorf("stat parquet: %w", err)
	}

	pf, err := parquet.OpenFile(f, info.Size())
	if err != nil {
		return nil, fmt.Errorf("parse parquet %s: %w", path, err)
	}

	fields := pf.Schema().Fields()
	names := make([]string, len(fields))
	for i, fl := range fields {
		names[i] = fl.Name()
	}

	// Index the columns we care about by their leaf position. The
	// feature tables written by ai/scripts/*.py are flat (one leaf per
	// top-level field), which keeps field index == column-chunk index.
	wanted := map[int]string{}
	for i, n := range names {
		if n == KeyColumn {
			wanted[i] = n
			continue
		}
		for _, w := range want {
			if n == w {
				wanted[i] = n
				break
			}
		}
	}

	tbl := &Table{
		Rows:    int(pf.NumRows()),
		Numeric: map[string][]float64{},
		Names:   names,
	}
	for _, n := range wanted {
		if n == KeyColumn {
			tbl.HasKey = true
			tbl.Keys = make([]string, 0, tbl.Rows)
			continue
		}
		tbl.Numeric[n] = make([]float64, 0, tbl.Rows)
	}

	// Row groups are laid out sequentially, so appending per group in
	// file order reconstructs the original row order — which the split
	// bucketing and the pred/target pairing both depend on.
	for _, rg := range pf.RowGroups() {
		for _, cc := range rg.ColumnChunks() {
			name, ok := wanted[cc.Column()]
			if !ok {
				continue
			}
			if err := readChunk(cc, name, tbl); err != nil {
				return nil, fmt.Errorf("column %q: %w", name, err)
			}
		}
	}
	return tbl, nil
}

// readChunk appends every value in one column chunk to the table.
func readChunk(cc parquet.ColumnChunk, name string, tbl *Table) error {
	pages := cc.Pages()
	defer func() { _ = pages.Close() }()

	buf := make([]parquet.Value, 512)
	for {
		pg, err := pages.ReadPage()
		if err != nil {
			if errors.Is(err, io.EOF) {
				return nil
			}
			return fmt.Errorf("read page: %w", err)
		}
		vr := pg.Values()
		for {
			n, err := vr.ReadValues(buf)
			for i := range n {
				if name == KeyColumn {
					tbl.Keys = append(tbl.Keys, valueToString(buf[i]))
				} else {
					tbl.Numeric[name] = append(tbl.Numeric[name], valueToFloat(buf[i]))
				}
			}
			if err != nil {
				if errors.Is(err, io.EOF) {
					break
				}
				return fmt.Errorf("read values: %w", err)
			}
		}
	}
}

// valueToFloat widens a parquet value to float64, mapping NULL to NaN
// the way pandas does when a nullable column is cast to a float dtype.
func valueToFloat(v parquet.Value) float64 {
	if v.IsNull() {
		return math.NaN()
	}
	switch v.Kind() {
	case parquet.Float:
		return float64(v.Float())
	case parquet.Double:
		return v.Double()
	case parquet.Int32:
		return float64(v.Int32())
	case parquet.Int64:
		return float64(v.Int64())
	case parquet.Boolean:
		if v.Boolean() {
			return 1
		}
		return 0
	default:
		return math.NaN()
	}
}

// valueToString renders a parquet value the way pandas' .astype(str)
// would for the key column. Integer and byte-array keys — the two forms
// the training scripts emit — round-trip exactly.
func valueToString(v parquet.Value) string {
	if v.IsNull() {
		return ""
	}
	switch v.Kind() {
	case parquet.ByteArray, parquet.FixedLenByteArray:
		return string(v.ByteArray())
	case parquet.Int32:
		return strconv.FormatInt(int64(v.Int32()), 10)
	case parquet.Int64:
		return strconv.FormatInt(v.Int64(), 10)
	case parquet.Boolean:
		if v.Boolean() {
			return "True"
		}
		return "False"
	default:
		return v.String()
	}
}
