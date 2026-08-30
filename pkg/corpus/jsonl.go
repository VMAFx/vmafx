// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/jsonl.go — corpus JSONL reader / writer.
//
// Port of corpus.write_jsonl / read_jsonl / _upgrade_row_in_place. The writer
// emits json.dumps(row, sort_keys=True) + "\n" per row (Python uses os.linesep,
// which is "\n" on every platform vmaf-tune supports); the reader tolerates the
// bare NaN / Infinity tokens CPython writes.

package corpus

import (
	"bufio"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"

	"github.com/VMAFx/vmafx/pkg/pyjson"
)

// WriteRowLine renders one corpus row exactly as the Python writer's
// json.dumps(row, sort_keys=True) does — no trailing newline.
//
// The row schema carries NaN in the canonical-6 columns whenever libvmaf did
// not expose a pooled feature, which encoding/json refuses to marshal at all,
// so every corpus write goes through the CPython-compatible encoder.
func WriteRowLine(row map[string]any) (string, error) {
	return pyjson.MarshalSorted(row)
}

// WriteJSONL writes rows to path, one JSON object per line, and returns the
// row count. Parent directories are created as needed.
func WriteJSONL(rows []map[string]any, path string) (int, error) {
	if dir := filepath.Dir(path); dir != "" {
		if err := os.MkdirAll(dir, 0o750); err != nil {
			return 0, fmt.Errorf("create corpus output dir: %w", err)
		}
	}
	// 0o600: corpus rows carry source paths that can leak dataset
	// identifiers; restrict to the owner.
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o600) // #nosec G304 -- operator-supplied --output path.
	if err != nil {
		return 0, fmt.Errorf("open corpus output: %w", err)
	}
	defer func() { _ = f.Close() }()

	w := bufio.NewWriter(f)
	n := 0
	for _, row := range rows {
		line, mErr := WriteRowLine(row)
		if mErr != nil {
			return n, fmt.Errorf("encode corpus row %d: %w", n, mErr)
		}
		if _, wErr := w.WriteString(line + "\n"); wErr != nil {
			return n, fmt.Errorf("write corpus row %d: %w", n, wErr)
		}
		n++
	}
	if err := w.Flush(); err != nil {
		return n, fmt.Errorf("flush corpus output: %w", err)
	}
	if err := f.Sync(); err != nil {
		return n, fmt.Errorf("sync corpus output: %w", err)
	}
	return n, nil
}

// ReadJSONL reads a corpus JSONL with forward / backward schema compatibility.
//
// Rows whose schema_version <= 2 predate the canonical-6 column addition
// (ADR-0366). When upgradeToCurrent is true (the Python default), the reader
// fills the missing v3 columns with NaN so downstream consumers can treat every
// row as v3-shaped. schema_version itself is not rewritten — the original value
// is preserved so callers that want to filter < 3 still can.
func ReadJSONL(path string, upgradeToCurrent bool) ([]map[string]any, error) {
	data, err := os.ReadFile(path) // #nosec G304 -- operator-supplied corpus path.
	if err != nil {
		return nil, fmt.Errorf("read corpus jsonl: %w", err)
	}
	var rows []map[string]any
	for lineno, raw := range strings.Split(string(data), "\n") {
		line := strings.TrimSpace(raw)
		if line == "" {
			continue
		}
		var decoded any
		if uErr := json.Unmarshal([]byte(pyjson.SanitizeNonFinite(line)), &decoded); uErr != nil {
			return nil, fmt.Errorf("corpus jsonl line %d: %w", lineno+1, uErr)
		}
		row, ok := pyjson.ResolveSentinels(decoded).(map[string]any)
		if !ok {
			return nil, fmt.Errorf("corpus jsonl line %d: row is not an object", lineno+1)
		}
		normalizeNumbers(row)
		if upgradeToCurrent {
			UpgradeRow(row)
		}
		rows = append(rows, row)
	}
	return rows, nil
}

// normalizeNumbers converts json.Number-free float64 values that are exactly
// integral and belong to an integer column back to int, so a round-trip through
// ReadJSONL -> WriteJSONL reproduces the original token (encoding/json decodes
// every JSON number as float64, which would otherwise re-render 3 as "3.0").
func normalizeNumbers(row map[string]any) {
	for _, key := range integerRowKeys {
		v, ok := row[key]
		if !ok {
			continue
		}
		f, isFloat := v.(float64)
		if !isFloat {
			continue
		}
		if f == math.Trunc(f) && !math.IsInf(f, 0) {
			row[key] = int(f)
		}
	}
}

// integerRowKeys are the row columns Python writes as Python ints.
var integerRowKeys = []string{
	"schema_version",
	"width",
	"height",
	"crf",
	"encode_size_bytes",
	"exit_status",
	"shot_count",
}

// UpgradeRow fills missing v3 canonical-6 columns with NaN on legacy rows.
//
// Current-schema rows are also defensively backfilled: a partial write would
// otherwise crash positional consumers.
func UpgradeRow(row map[string]any) {
	for _, key := range Canonical6MeanKeys {
		if _, ok := row[key]; !ok {
			row[key] = math.NaN()
		}
	}
	for _, key := range Canonical6StdKeys {
		if _, ok := row[key]; !ok {
			row[key] = math.NaN()
		}
	}
}
