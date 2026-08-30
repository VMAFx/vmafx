// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package benchmark is the Go port of vmaftune.benchmark — the Phase-G
// cross-codec corpus summary behind `vmaf-tune benchmark`.
//
// It answers the standard post-sweep question: "which encoder hit the target
// quality at the lowest bitrate?" It deliberately runs no ffmpeg and no
// libvmaf; a Phase-A corpus JSONL written by the `corpus` subcommand is the
// only input, and stays the source of truth.
//
// Parity notes (the Python semantics this port reproduces exactly):
//
//   - Row values arrive from JSON as json.Number, so an integer literal stays
//     an integer on the way back out. CPython's json module makes the same
//     distinction, and it is visible in the rendered payloads (`"crf": 23` vs
//     `"crf": 23.0`).
//   - Coercion is CPython's, not Go's: float("23") and int("0") succeed,
//     int("0.0") does not, and a non-finite value is treated as missing.
//   - min/max over a key take the FIRST extremal element, matching CPython's
//     min()/max(); the final sort is stable, matching sorted().
//
// ADR-0705 / ADR-0730 / ADR-0770: staged Go port of vmaf-tune.
package benchmark

import (
	"bufio"
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"

	pyjson "github.com/VMAFx/vmafx/internal/pyjsonstrict"
)

// Row is one Phase-A corpus record. Values are whatever the JSONL carried,
// decoded with json.Decoder.UseNumber so numeric literals keep their
// int-vs-float identity.
type Row map[string]any

// Summary is one encoder's best matched-quality corpus point — the Go
// equivalent of benchmark.BenchmarkSummary.
type Summary struct {
	// Encoder is the corpus row's encoder token.
	Encoder string

	// Status is "ok" when the encoder cleared TargetVMAF somewhere in the
	// corpus, "unmet" when the reported point is its closest miss.
	Status string

	// Rows is how many eligible corpus rows this encoder contributed.
	Rows int

	// SourceCount / PresetCount are the distinct src / preset values across
	// those rows.
	SourceCount int
	PresetCount int

	// BestRow is the selected corpus row, kept whole so renderers can read
	// fields the Summary does not promote.
	BestRow Row

	// TargetVMAF is the threshold the caller asked for.
	TargetVMAF float64

	// Margin is the selected row's VMAF minus TargetVMAF (negative when
	// Status is "unmet").
	Margin float64

	// BitratekBps is the selected row's bitrate.
	BitratekBps float64

	// BitrateDeltaPct is the percentage difference against the baseline
	// encoder's bitrate. Nil when no baseline could be resolved (no encoder
	// cleared the target) or the baseline bitrate is not positive.
	BitrateDeltaPct *float64

	// EncodeFPS / ScoreFPS are means over this encoder's eligible rows,
	// counting only positive finite samples. Nil when no row supplied one.
	EncodeFPS *float64
	ScoreFPS  *float64
}

// ---------------------------------------------------------------------------
// CPython coercion helpers
// ---------------------------------------------------------------------------

// finiteFloat mirrors benchmark._finite_float: CPython's float() over the
// value, then a finiteness check, with any failure reported as "absent".
func finiteFloat(v any) (float64, bool) {
	f, ok := toFloat(v)
	if !ok || math.IsNaN(f) || math.IsInf(f, 0) {
		return 0, false
	}
	return f, true
}

// toFloat is CPython's float() over the JSON value shapes a corpus row can
// hold. float(None) and float([]) raise, so those report failure.
func toFloat(v any) (float64, bool) {
	switch t := v.(type) {
	case json.Number:
		f, err := t.Float64()
		return f, err == nil
	case float64:
		return t, true
	case int:
		return float64(t), true
	case int64:
		return float64(t), true
	case bool:
		if t {
			return 1, true
		}
		return 0, true
	case string:
		// CPython's float() strips surrounding whitespace and accepts the
		// "nan" / "inf" spellings Go's ParseFloat also takes.
		f, err := strconv.ParseFloat(strings.TrimSpace(t), 64)
		return f, err == nil
	default:
		return 0, false
	}
}

// toInt is CPython's int() over the same shapes: floats truncate toward zero,
// strings must be an integer literal (int("0.0") raises), None raises.
func toInt(v any) (int, bool) {
	switch t := v.(type) {
	case json.Number:
		if i, err := strconv.ParseInt(t.String(), 10, 64); err == nil {
			return int(i), true
		}
		f, err := t.Float64()
		if err != nil {
			return 0, false
		}
		return int(math.Trunc(f)), true
	case float64:
		return int(math.Trunc(t)), true
	case int:
		return t, true
	case int64:
		return int(t), true
	case bool:
		if t {
			return 1, true
		}
		return 0, true
	case string:
		i, err := strconv.ParseInt(strings.TrimSpace(t), 10, 64)
		return int(i), err == nil
	default:
		return 0, false
	}
}

// pyStr mirrors CPython's str() for the row values this package stringifies —
// str(row.get("encoder", "")) and friends. A missing key already resolved to
// the "" default before the call.
func pyStr(v any) string {
	switch t := v.(type) {
	case nil:
		return "None"
	case string:
		return t
	case bool:
		if t {
			return "True"
		}
		return "False"
	case json.Number:
		return numberStr(t)
	case float64:
		return pyjson.Repr(t)
	default:
		return fmt.Sprint(v)
	}
}

// numberStr renders a json.Number the way str() would render the Python object
// json.loads produced from that literal: an int for an integral literal,
// otherwise repr() of the float.
func numberStr(n json.Number) string {
	if i, err := strconv.ParseInt(n.String(), 10, 64); err == nil {
		return strconv.FormatInt(i, 10)
	}
	if f, err := n.Float64(); err == nil {
		return pyjson.Repr(f)
	}
	return n.String()
}

// jsonValue normalises a decoded row value into the shapes pyjson accepts,
// leaving int-vs-float identity intact.
func jsonValue(v any) any {
	if n, ok := v.(json.Number); ok {
		if i, err := strconv.ParseInt(n.String(), 10, 64); err == nil {
			return i
		}
		if f, err := n.Float64(); err == nil {
			return f
		}
		return n.String()
	}
	return v
}

// get returns row[key], or def when the key is absent — CPython's dict.get.
func (r Row) get(key string, def any) any {
	if v, ok := r[key]; ok {
		return v
	}
	return def
}

// ---------------------------------------------------------------------------
// Corpus loading
// ---------------------------------------------------------------------------

// maxCorpusLine bounds a single JSONL record. Corpus rows are flat objects of
// a few hundred bytes; a multi-megabyte "line" means the file is not JSONL
// (a pretty-printed JSON document, say) and bufio.Scanner would otherwise
// fail with an opaque token-too-long error.
const maxCorpusLine = 4 << 20 // 4 MiB

// LoadCorpusJSONL streams rows from a JSONL file written by the `corpus`
// subcommand — the Go port of recommend.load_corpus_jsonl. Blank lines are
// skipped; every other line must be a JSON object.
func LoadCorpusJSONL(path string) ([]Row, error) {
	f, err := os.Open(path) // #nosec G304 -- path is a CLI flag, not remote input
	if err != nil {
		return nil, err
	}
	defer func() { _ = f.Close() }()
	return ParseCorpusJSONL(f)
}

// ParseCorpusJSONL parses JSONL rows from r. Split out from LoadCorpusJSONL so
// tests (and future callers streaming from object storage) do not need a file.
func ParseCorpusJSONL(r io.Reader) ([]Row, error) {
	var rows []Row
	scanner := bufio.NewScanner(r)
	scanner.Buffer(make([]byte, 0, 64*1024), maxCorpusLine)

	for lineNo := 1; scanner.Scan(); lineNo++ {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		dec := json.NewDecoder(strings.NewReader(line))
		dec.UseNumber()
		var row Row
		if err := dec.Decode(&row); err != nil {
			return nil, fmt.Errorf("line %d: %w", lineNo, err)
		}
		rows = append(rows, row)
	}
	if err := scanner.Err(); err != nil {
		return nil, err
	}
	return rows, nil
}

// ---------------------------------------------------------------------------
// Summarisation
// ---------------------------------------------------------------------------

// eligibleRows keeps successful rows with a finite VMAF and bitrate.
func eligibleRows(rows []Row) []Row {
	out := make([]Row, 0, len(rows))
	for _, row := range rows {
		status, ok := toInt(row.get("exit_status", 0))
		if !ok || status != 0 {
			continue
		}
		if _, ok := finiteFloat(row["vmaf_score"]); !ok {
			continue
		}
		if _, ok := finiteFloat(row["bitrate_kbps"]); !ok {
			continue
		}
		out = append(out, row)
	}
	return out
}

// meanPositive averages the positive finite samples, reporting nil when there
// are none.
func meanPositive(values []*float64) *float64 {
	sum, n := 0.0, 0
	for _, v := range values {
		if v == nil || *v <= 0 || math.IsNaN(*v) || math.IsInf(*v, 0) {
			continue
		}
		sum += *v
		n++
	}
	if n == 0 {
		return nil
	}
	out := sum / float64(n)
	return &out
}

// rowEncodeFPS is source seconds per second of encode wall time.
func rowEncodeFPS(row Row) *float64 {
	duration, okD := finiteFloat(row["duration_s"])
	encodeMS, okE := finiteFloat(row["encode_time_ms"])
	if !okD || !okE || duration <= 0 || encodeMS <= 0 {
		return nil
	}
	out := duration / (encodeMS / 1000.0)
	return &out
}

// rowScoreFPS is scored frames per second of scoring wall time.
func rowScoreFPS(row Row) *float64 {
	duration, okD := finiteFloat(row["duration_s"])
	scoreMS, okS := finiteFloat(row["score_time_ms"])
	framerate, okF := finiteFloat(row["framerate"])
	if !okD || !okS || !okF || duration <= 0 || scoreMS <= 0 || framerate <= 0 {
		return nil
	}
	out := (duration * framerate) / (scoreMS / 1000.0)
	return &out
}

// bestRow picks the reported point for one encoder: the lowest-bitrate row
// clearing the target, else the closest miss by VMAF.
//
// The tie-breaks mirror CPython exactly. Clearing rows are ordered by
// (bitrate, -vmaf, crf) and non-clearing rows by (vmaf, -bitrate); in both
// cases the FIRST extremal row wins, because CPython's min()/max() keep the
// first element with the extremal key.
func bestRow(rows []Row, targetVMAF float64) (string, Row) {
	var clearing []Row
	for _, row := range rows {
		v, _ := finiteFloat(row["vmaf_score"])
		if v >= targetVMAF {
			clearing = append(clearing, row)
		}
	}

	if len(clearing) > 0 {
		best := clearing[0]
		bestKey := clearingKey(best)
		for _, row := range clearing[1:] {
			if k := clearingKey(row); lessTriple(k, bestKey) {
				best, bestKey = row, k
			}
		}
		return "ok", best
	}

	best := rows[0]
	bestKey := unmetKey(best)
	for _, row := range rows[1:] {
		if k := unmetKey(row); lessPair(bestKey, k) {
			best, bestKey = row, k
		}
	}
	return "unmet", best
}

// clearingKey is CPython's (bitrate_kbps, -vmaf_score, int(crf or 0)) sort key.
func clearingKey(row Row) [3]float64 {
	bitrate, _ := finiteFloat(row["bitrate_kbps"])
	vmaf, _ := finiteFloat(row["vmaf_score"])
	// CPython evaluates int(row.get("crf", 0)); a present-but-uncoercible crf
	// would raise there. Treating it as 0 keeps the summary usable on a
	// malformed corpus instead of aborting the whole run, and only affects
	// ordering between rows already tied on bitrate AND VMAF.
	crf, _ := toInt(row.get("crf", 0))
	return [3]float64{bitrate, -vmaf, float64(crf)}
}

// unmetKey is CPython's (vmaf_score, -bitrate_kbps) sort key.
func unmetKey(row Row) [2]float64 {
	vmaf, _ := finiteFloat(row["vmaf_score"])
	bitrate, _ := finiteFloat(row["bitrate_kbps"])
	return [2]float64{vmaf, -bitrate}
}

// lessTriple is lexicographic < over a 3-tuple.
func lessTriple(a, b [3]float64) bool {
	for i := range a {
		if a[i] != b[i] {
			return a[i] < b[i]
		}
	}
	return false
}

// lessPair is lexicographic < over a 2-tuple.
func lessPair(a, b [2]float64) bool {
	for i := range a {
		if a[i] != b[i] {
			return a[i] < b[i]
		}
	}
	return false
}

// ErrNoEligibleRows is returned when the corpus holds no successful row with a
// finite VMAF and bitrate.
var ErrNoEligibleRows = errors.New("no successful finite corpus rows to benchmark")

// ErrNoEncoderNames is returned when eligible rows carry no encoder token.
var ErrNoEncoderNames = errors.New("corpus rows do not include encoder names")

// Summarize returns the best matched-quality point per encoder.
//
// baselineEncoder selects the encoder that bitrate deltas are measured
// against; pass "" to use the lowest-bitrate encoder that cleared the target.
func Summarize(rows []Row, targetVMAF float64, baselineEncoder string) ([]Summary, error) {
	eligible := eligibleRows(rows)
	if len(eligible) == 0 {
		return nil, ErrNoEligibleRows
	}

	byEncoder := map[string][]Row{}
	for _, row := range eligible {
		enc := pyStr(row.get("encoder", ""))
		if enc == "" {
			continue
		}
		byEncoder[enc] = append(byEncoder[enc], row)
	}
	if len(byEncoder) == 0 {
		return nil, ErrNoEncoderNames
	}

	encoders := make([]string, 0, len(byEncoder))
	for enc := range byEncoder {
		encoders = append(encoders, enc)
	}
	sort.Strings(encoders)

	raw := make([]Summary, 0, len(encoders))
	for _, enc := range encoders {
		group := byEncoder[enc]
		status, row := bestRow(group, targetVMAF)
		bitrate, _ := finiteFloat(row["bitrate_kbps"])
		vmaf, _ := finiteFloat(row["vmaf_score"])

		srcs := map[string]struct{}{}
		presets := map[string]struct{}{}
		encodeSamples := make([]*float64, 0, len(group))
		scoreSamples := make([]*float64, 0, len(group))
		for _, r := range group {
			srcs[pyStr(r.get("src", ""))] = struct{}{}
			presets[pyStr(r.get("preset", ""))] = struct{}{}
			encodeSamples = append(encodeSamples, rowEncodeFPS(r))
			scoreSamples = append(scoreSamples, rowScoreFPS(r))
		}

		raw = append(raw, Summary{
			Encoder:     enc,
			Status:      status,
			Rows:        len(group),
			SourceCount: len(srcs),
			PresetCount: len(presets),
			BestRow:     row,
			TargetVMAF:  targetVMAF,
			Margin:      vmaf - targetVMAF,
			BitratekBps: bitrate,
			EncodeFPS:   meanPositive(encodeSamples),
			ScoreFPS:    meanPositive(scoreSamples),
		})
	}

	baseline, err := resolveBaseline(raw, baselineEncoder)
	if err != nil {
		return nil, err
	}
	if baseline != nil && baseline.BitratekBps > 0 {
		base := baseline.BitratekBps
		for i := range raw {
			delta := ((raw[i].BitratekBps - base) / base) * 100.0
			raw[i].BitrateDeltaPct = &delta
		}
	}

	sortSummaries(raw)
	return raw, nil
}

// resolveBaseline picks the reference encoder for bitrate deltas. A named
// encoder that is absent from the corpus is an error; with no name, the
// lowest-bitrate "ok" encoder wins, and nil means no encoder cleared.
func resolveBaseline(summaries []Summary, baselineEncoder string) (*Summary, error) {
	if baselineEncoder != "" {
		for i := range summaries {
			if summaries[i].Encoder == baselineEncoder {
				return &summaries[i], nil
			}
		}
		return nil, fmt.Errorf("baseline encoder %q not present in corpus", baselineEncoder)
	}

	var best *Summary
	for i := range summaries {
		if summaries[i].Status != "ok" {
			continue
		}
		if best == nil || summaries[i].BitratekBps < best.BitratekBps {
			best = &summaries[i]
		}
	}
	return best, nil
}

// sortSummaries orders the report: cleared encoders first, then by ascending
// bitrate, then by encoder name.
func sortSummaries(summaries []Summary) {
	sort.SliceStable(summaries, func(i, j int) bool {
		si, sj := summaries[i], summaries[j]
		oki, okj := 0, 0
		if si.Status != "ok" {
			oki = 1
		}
		if sj.Status != "ok" {
			okj = 1
		}
		if oki != okj {
			return oki < okj
		}
		if si.BitratekBps != sj.BitratekBps {
			return si.BitratekBps < sj.BitratekBps
		}
		return si.Encoder < sj.Encoder
	})
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// ToPayload builds the JSON-serialisable payload — benchmark.summaries_to_dicts.
func ToPayload(summaries []Summary) []any {
	out := make([]any, 0, len(summaries))
	for _, item := range summaries {
		row := item.BestRow
		out = append(out, map[string]any{
			"encoder":           item.Encoder,
			"status":            item.Status,
			"target_vmaf":       item.TargetVMAF,
			"margin":            item.Margin,
			"bitrate_kbps":      item.BitratekBps,
			"bitrate_delta_pct": optFloat(item.BitrateDeltaPct),
			"rows":              item.Rows,
			"source_count":      item.SourceCount,
			"preset_count":      item.PresetCount,
			"encode_fps":        optFloat(item.EncodeFPS),
			"score_fps":         optFloat(item.ScoreFPS),
			"best": map[string]any{
				"src":          jsonValue(row.get("src", "")),
				"preset":       jsonValue(row.get("preset", "")),
				"crf":          jsonValue(row.get("crf", nil)),
				"vmaf_score":   jsonValue(row["vmaf_score"]),
				"bitrate_kbps": jsonValue(row["bitrate_kbps"]),
				"vmaf_model":   jsonValue(row.get("vmaf_model", "")),
			},
		})
	}
	return out
}

// optFloat unwraps an optional float into the `any` pyjson expects, mapping
// absence onto JSON null.
func optFloat(v *float64) any {
	if v == nil {
		return nil
	}
	return *v
}

// RenderJSON renders the summaries as stable pretty RFC 8259 JSON (ADR-0988),
// with the trailing newline the Python renderer appends.
func RenderJSON(summaries []Summary) (string, error) {
	s, err := pyjson.Marshal(ToPayload(summaries), pyjson.NaNAsNull)
	if err != nil {
		return "", err
	}
	return s + "\n", nil
}

// csvFieldNames is the DictWriter header, in Python's declaration order.
var csvFieldNames = []string{
	"encoder", "status", "target_vmaf", "vmaf_score", "margin",
	"bitrate_kbps", "bitrate_delta_pct", "preset", "crf", "rows",
	"source_count", "preset_count", "encode_fps", "score_fps",
}

// RenderCSV renders the summaries as CSV.
//
// Python's csv module defaults to the "excel" dialect, whose line terminator
// is CRLF — so this writer sets UseCRLF. Getting that wrong would change every
// byte of every line ending relative to the Python output.
func RenderCSV(summaries []Summary) (string, error) {
	var sb strings.Builder
	w := csv.NewWriter(&sb)
	w.UseCRLF = true

	if err := w.Write(csvFieldNames); err != nil {
		return "", err
	}
	for _, item := range summaries {
		row := item.BestRow
		vmaf, _ := finiteFloat(row["vmaf_score"])
		if err := w.Write([]string{
			item.Encoder,
			item.Status,
			fmt.Sprintf("%.3f", item.TargetVMAF),
			fmt.Sprintf("%.3f", vmaf),
			fmt.Sprintf("%.3f", item.Margin),
			fmt.Sprintf("%.3f", item.BitratekBps),
			formatOptional(item.BitrateDeltaPct),
			csvCell(row.get("preset", "")),
			csvCell(row.get("crf", "")),
			strconv.Itoa(item.Rows),
			strconv.Itoa(item.SourceCount),
			strconv.Itoa(item.PresetCount),
			formatOptional(item.EncodeFPS),
			formatOptional(item.ScoreFPS),
		}); err != nil {
			return "", err
		}
	}
	w.Flush()
	if err := w.Error(); err != nil {
		return "", err
	}
	return sb.String(), nil
}

// csvCell mirrors the csv module's value conversion: None becomes the empty
// string, everything else goes through str().
func csvCell(v any) string {
	if v == nil {
		return ""
	}
	return pyStr(v)
}

// RenderMarkdown renders the summaries as a compact Markdown table.
func RenderMarkdown(summaries []Summary) string {
	lines := []string{
		"| Encoder | Status | VMAF | kbps | Δ kbps | Preset | CRF | Rows | Encode fps | Score fps |",
		"| --- | --- | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |",
	}
	for _, item := range summaries {
		row := item.BestRow
		vmaf, _ := finiteFloat(row["vmaf_score"])
		lines = append(lines, fmt.Sprintf(
			"| %s | %s | %.3f | %.1f | %s | %s | %s | %d | %s | %s |",
			item.Encoder,
			item.Status,
			vmaf,
			item.BitratekBps,
			formatOptional(item.BitrateDeltaPct),
			// f"{row.get('preset', '')}" — an f-string, so a present-but-null
			// value stringifies as "None" rather than blank. Mirrored.
			pyStr(row.get("preset", "")),
			pyStr(row.get("crf", "")),
			item.Rows,
			formatOptional(item.EncodeFPS),
			formatOptional(item.ScoreFPS),
		))
	}
	return strings.Join(lines, "\n") + "\n"
}

// formatOptional renders an optional float with three decimals, or the empty
// string for absent / non-finite values.
func formatOptional(v *float64) string {
	if v == nil || math.IsNaN(*v) || math.IsInf(*v, 0) {
		return ""
	}
	return fmt.Sprintf("%.3f", *v)
}

// Render dispatches to the requested renderer. Formats: json, csv, markdown.
func Render(summaries []Summary, format string) (string, error) {
	switch format {
	case "json":
		return RenderJSON(summaries)
	case "csv":
		return RenderCSV(summaries)
	case "markdown":
		return RenderMarkdown(summaries), nil
	default:
		return "", fmt.Errorf("unknown benchmark format %q", format)
	}
}
