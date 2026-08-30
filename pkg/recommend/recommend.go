// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package recommend is the Go port of tools/vmaf-tune/src/vmaftune/
// recommend.py — predicate-driven CRF recommendation over a corpus.
//
// The corpus subcommand already produces (preset, crf, bitrate_kbps,
// vmaf_score) rows. This package re-uses those rows and applies one of two
// user-supplied predicates:
//
//   - target VMAF T — return the row with the SMALLEST crf whose vmaf_score
//     >= T (smaller CRF = higher quality, so the smallest passing CRF is the
//     best quality that clears the gate). Falls back to the highest-VMAF row
//     when nothing clears the bar, so the user sees the closest miss rather
//     than an empty result.
//   - target bitrate B — return the row whose bitrate_kbps is closest to B,
//     ties broken toward the lower CRF.
//
// Exactly one target must be set. Implements buckets #4 and #5 from the
// Research-0061 capability audit.
//
// The uncertainty-aware extension (ADR-0279) adds an interval-aware search
// that short-circuits at the first row whose conformal lower bound already
// clears the target, provided that row's interval is tight enough for the
// conservative bound to be a faithful proxy (Lei et al. 2018 Theorem 2.2).
// It changes which encodes get PROBED, never which get SHIPPED — the
// production-flip gate stays in pkg/predictor.
package recommend

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"strconv"
	"strings"

	"github.com/VMAFx/vmafx/pkg/uncertainty"
)

// Row is one corpus JSONL record. Unknown keys are preserved in Extra so the
// CLI can echo the winning row back verbatim, matching the Python handler
// which dumps the raw dict.
type Row map[string]any

// Interval is a per-row conformal interval embedded under "vmaf_interval".
type Interval struct {
	Low  float64
	High float64
}

// Request describes a point-estimate predicate. Exactly one of TargetVMAF /
// TargetBitrateKbps must be set.
type Request struct {
	TargetVMAF        *float64
	TargetBitrateKbps *float64
	Encoder           string
	Preset            string
}

// Result is the winning row plus the predicate that picked it.
type Result struct {
	Row       Row
	Predicate string
	// Margin is the predicate-specific distance from the target:
	// vmaf_score - target for target-vmaf, bitrate_kbps - target for
	// target-bitrate (signed in both cases).
	Margin float64
}

// ValidateRequest enforces the mutually-exclusive target.
func ValidateRequest(req Request) error {
	hasVMAF := req.TargetVMAF != nil
	hasBitrate := req.TargetBitrateKbps != nil
	if hasVMAF && hasBitrate {
		return errors.New(
			"--target-vmaf and --target-bitrate are mutually exclusive; specify exactly one")
	}
	if !hasVMAF && !hasBitrate {
		return errors.New("missing target: pass --target-vmaf or --target-bitrate")
	}
	return nil
}

// numField reads a numeric field, tolerating the JSON number / string /
// missing shapes real corpus rows carry.
func numField(row Row, key string) (float64, bool) {
	raw, ok := row[key]
	if !ok || raw == nil {
		return 0, false
	}
	switch v := raw.(type) {
	case float64:
		return v, true
	case int:
		return float64(v), true
	case json.Number:
		f, err := v.Float64()
		return f, err == nil
	default:
		return 0, false
	}
}

// strField reads a string field.
func strField(row Row, key string) (string, bool) {
	raw, ok := row[key]
	if !ok {
		return "", false
	}
	s, ok := raw.(string)
	return s, ok
}

// intField reads an integer field.
func intField(row Row, key string) (int, bool) {
	f, ok := numField(row, key)
	if !ok {
		return 0, false
	}
	return int(f), true
}

// eligible drops rows that fail the encoder / preset filter, that record a
// non-zero exit status, or whose vmaf_score is missing or non-finite.
func eligible(rows []Row, encoder, preset string) []Row {
	out := make([]Row, 0, len(rows))
	for _, row := range rows {
		if encoder != "" {
			if got, _ := strField(row, "encoder"); got != encoder {
				continue
			}
		}
		if preset != "" {
			if got, _ := strField(row, "preset"); got != preset {
				continue
			}
		}
		if status, ok := intField(row, "exit_status"); ok && status != 0 {
			continue
		}
		score, ok := numField(row, "vmaf_score")
		if !ok || math.IsNaN(score) || math.IsInf(score, 0) {
			continue
		}
		out = append(out, row)
	}
	return out
}

// PickTargetVMAF returns the smallest-CRF row whose VMAF clears target,
// falling back to the highest-VMAF row when nothing does.
func PickTargetVMAF(rows []Row, target float64) (Result, error) {
	if len(rows) == 0 {
		return Result{}, errors.New("no eligible rows to evaluate (after filtering)")
	}

	var winner Row
	bestCRF, bestScore := 0, 0.0
	for _, row := range rows {
		score, _ := numField(row, "vmaf_score")
		if score < target {
			continue
		}
		crf, _ := intField(row, "crf")
		// min by (crf, -vmaf_score): smallest CRF wins; ties break to the
		// higher score for determinism.
		if winner == nil || crf < bestCRF || (crf == bestCRF && score > bestScore) {
			winner, bestCRF, bestScore = row, crf, score
		}
	}
	if winner != nil {
		return Result{
			Row:       winner,
			Predicate: fmt.Sprintf("target_vmaf>=%s", formatTarget(target)),
			Margin:    bestScore - target,
		}, nil
	}

	// Nothing clears the bar — return the closest miss from below.
	winner = rows[0]
	bestScore, _ = numField(winner, "vmaf_score")
	for _, row := range rows[1:] {
		if score, _ := numField(row, "vmaf_score"); score > bestScore {
			winner, bestScore = row, score
		}
	}
	return Result{
		Row:       winner,
		Predicate: fmt.Sprintf("target_vmaf>=%s (UNMET)", formatTarget(target)),
		Margin:    bestScore - target,
	}, nil
}

// PickTargetBitrate returns the row whose bitrate is closest to targetKbps.
// Ties on distance go to the lower CRF (higher quality), matching the
// producer intent "best quality fitting under the bitrate cap".
func PickTargetBitrate(rows []Row, targetKbps float64) (Result, error) {
	if len(rows) == 0 {
		return Result{}, errors.New("no eligible rows to evaluate (after filtering)")
	}
	var winner Row
	bestDist, bestCRF, bestBitrate := 0.0, 0, 0.0
	for _, row := range rows {
		bitrate, _ := numField(row, "bitrate_kbps")
		crf, _ := intField(row, "crf")
		dist := math.Abs(bitrate - targetKbps)
		if winner == nil || dist < bestDist || (dist == bestDist && crf < bestCRF) {
			winner, bestDist, bestCRF, bestBitrate = row, dist, crf, bitrate
		}
	}
	return Result{
		Row:       winner,
		Predicate: fmt.Sprintf("|bitrate-%s|->min", formatTarget(targetKbps)),
		Margin:    bestBitrate - targetKbps,
	}, nil
}

// Recommend validates the request, filters the rows and applies the
// predicate.
func Recommend(rows []Row, req Request) (Result, error) {
	if err := ValidateRequest(req); err != nil {
		return Result{}, err
	}
	rows = eligible(rows, req.Encoder, req.Preset)
	if req.TargetVMAF != nil {
		return PickTargetVMAF(rows, *req.TargetVMAF)
	}
	return PickTargetBitrate(rows, *req.TargetBitrateKbps)
}

// UncertaintyRequest describes the interval-aware predicate.
type UncertaintyRequest struct {
	TargetVMAF float64
	Thresholds uncertainty.Thresholds
	Encoder    string
	Preset     string
	// SampleUncertainty overrides each row's embedded vmaf_interval block,
	// keyed by integer CRF. Used by callers that produce intervals
	// out-of-band (the deep-ensemble + conformal pipeline) and by tests.
	SampleUncertainty map[int]Interval
}

// UncertaintyResult adds the decision band and the visited count so callers
// can audit which short-circuit fired and how many rows were examined.
type UncertaintyResult struct {
	Row       Row
	Predicate string
	Margin    float64
	Decision  uncertainty.Decision
	// Visited is the number of rows examined before termination. With a
	// tight interval the search short-circuits at the first definitely-
	// clearing row, so Visited is typically well below len(rows). With a
	// wide interval the full scan runs and Visited == len(eligible rows).
	Visited int
}

// rowInterval resolves (point, low, high) for one row.
//
// Resolution order: the per-call SampleUncertainty override, then the row's
// embedded "vmaf_interval" block, then a NaN-bounded degenerate interval.
//
// The NaN bounds are deliberate: a zero-width (point, point, point) interval
// would classify as TIGHT and trigger a spurious short-circuit on a row whose
// "lower bound" is just the point estimate. NaN classifies as MIDDLE, which
// defers to the point-estimate recipe verbatim.
func rowInterval(row Row, req UncertaintyRequest) (point, low, high float64) {
	point, ok := numField(row, "vmaf_score")
	if !ok {
		return math.NaN(), math.NaN(), math.NaN()
	}
	if req.SampleUncertainty != nil {
		if crf, hasCRF := intField(row, "crf"); hasCRF {
			if override, hasOverride := req.SampleUncertainty[crf]; hasOverride {
				return point, override.Low, override.High
			}
		}
	}
	if raw, hasIV := row["vmaf_interval"]; hasIV {
		if block, isMap := raw.(map[string]any); isMap {
			lowRaw, hasLow := numField(Row(block), "low")
			highRaw, hasHigh := numField(Row(block), "high")
			if hasLow && hasHigh {
				return point, lowRaw, highRaw
			}
		}
	}
	return point, math.NaN(), math.NaN()
}

// PickTargetVMAFWithUncertainty is the interval-aware analogue of
// PickTargetVMAF.
//
// Iteration follows the input order (typically ascending CRF as produced by
// the coarse-to-fine search); callers who want a different traversal pre-sort.
//
// Decision rules:
//   - tight interval whose low >= target — promote immediately; the conformal
//     lower bound is a conservative proxy that already clears the bar.
//   - wide interval — refuse to short-circuit on any single row; fall through
//     to the full point-estimate scan, tagged "(UNCERTAIN)".
//   - middle band — defer to the native point-estimate predicate verbatim.
//   - every visited row's interval excludes the target — surface the
//     best-effort (highest-VMAF) row tagged "(UNMET, interval-excluded)".
func PickTargetVMAFWithUncertainty(rows []Row, req UncertaintyRequest) (UncertaintyResult, error) {
	rows = eligible(rows, req.Encoder, req.Preset)
	if len(rows) == 0 {
		return UncertaintyResult{}, errors.New("no eligible rows to evaluate (after filtering)")
	}

	target := req.TargetVMAF
	visited := 0
	var bestSoFar Row
	bestScore := math.Inf(-1)
	everyRowExcludes := true
	sawWide := false

	for _, row := range rows {
		visited++
		_, low, high := rowInterval(row, req)
		score, _ := numField(row, "vmaf_score")
		if score > bestScore {
			bestScore, bestSoFar = score, row
		}
		if !uncertainty.ExcludesTarget(low, high, target, 0.0) {
			everyRowExcludes = false
		}

		// Preserve NaN through to Classify so an uncalibrated row defers to
		// the MIDDLE band rather than being mis-read as a zero-width TIGHT.
		width := math.NaN()
		if !math.IsNaN(low) && !math.IsNaN(high) {
			width = math.Max(0.0, high-low)
		}
		decision, err := uncertainty.Classify(width, req.Thresholds)
		if err != nil {
			return UncertaintyResult{}, err
		}
		if decision == uncertainty.Wide {
			sawWide = true
		}
		if decision == uncertainty.Tight && low >= target {
			return UncertaintyResult{
				Row: row,
				Predicate: fmt.Sprintf("target_vmaf>=%s (TIGHT, low=%.3f)",
					formatTarget(target), low),
				Margin:   score - target,
				Decision: uncertainty.Tight,
				Visited:  visited,
			}, nil
		}
	}

	band := uncertainty.Middle
	if sawWide {
		band = uncertainty.Wide
	}

	if everyRowExcludes {
		return UncertaintyResult{
			Row: bestSoFar,
			Predicate: fmt.Sprintf("target_vmaf>=%s (UNMET, interval-excluded)",
				formatTarget(target)),
			Margin:   bestScore - target,
			Decision: band,
			Visited:  visited,
		}, nil
	}

	pointPick, err := PickTargetVMAF(rows, target)
	if err != nil {
		return UncertaintyResult{}, err
	}
	suffix := ""
	if band == uncertainty.Wide {
		suffix = " (UNCERTAIN)"
	}
	return UncertaintyResult{
		Row:       pointPick.Row,
		Predicate: pointPick.Predicate + suffix,
		Margin:    pointPick.Margin,
		Decision:  band,
		Visited:   visited,
	}, nil
}

// formatTarget renders a target the way Python's f-string renders a float, so
// the predicate strings the CLI prints match the Python originals byte for
// byte (93.0 -> "93.0", 93.5 -> "93.5", 5000.0 -> "5000.0").
//
// Python switches to exponential notation at |v| >= 1e16 and for very small
// magnitudes; this helper always uses positional notation. Both callers pass
// a VMAF target in [0, 100] or a bitrate in kbps, so the divergence is
// unreachable in practice.
func formatTarget(v float64) string {
	s := strconv.FormatFloat(v, 'f', -1, 64)
	if !strings.Contains(s, ".") {
		s += ".0"
	}
	return s
}

// LoadCorpusJSONL streams rows from a JSONL file written by the corpus
// subcommand. Blank lines are skipped; a malformed line is an error, because
// silently dropping corpus rows would bias the recommendation.
func LoadCorpusJSONL(path string) ([]Row, error) {
	// #nosec G304 -- path comes from the --from-corpus CLI flag.
	fh, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("recommend: open corpus %q: %w", path, err)
	}
	defer func() {
		if closeErr := fh.Close(); closeErr != nil {
			_ = closeErr
		}
	}()
	return ParseCorpusJSONL(fh)
}

// ParseCorpusJSONL parses JSONL rows from r. Split out from LoadCorpusJSONL
// so tests do not need a file on disk.
//
// Corpus rows legitimately carry bare NaN / Infinity / -Infinity tokens: a
// feature aggregate the run did not measure is NaN by design (ADR-0366), and
// Python's json.dumps — which writes every corpus in the tree — emits those
// tokens rather than null. Go's encoding/json rejects them outright, so each
// line is normalised first; see SanitizeNonFiniteTokens.
func ParseCorpusJSONL(r io.Reader) ([]Row, error) {
	var rows []Row
	scanner := bufio.NewScanner(r)
	// Corpus rows carry per-frame bisect samples and can be long; raise the
	// token cap well above bufio's 64 KiB default.
	scanner.Buffer(make([]byte, 0, 64*1024), 8*1024*1024)
	lineNo := 0
	for scanner.Scan() {
		lineNo++
		line := scanner.Bytes()
		if len(trimSpace(line)) == 0 {
			continue
		}
		var row Row
		if err := json.Unmarshal(SanitizeNonFiniteTokens(line), &row); err != nil {
			return nil, fmt.Errorf("recommend: corpus line %d: %w", lineNo, err)
		}
		rows = append(rows, row)
	}
	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("recommend: read corpus: %w", err)
	}
	return rows, nil
}

// nonFiniteTokens are Python's json.dumps spellings for the non-finite
// floats, longest first so "-Infinity" is matched before "Infinity".
var nonFiniteTokens = []string{"-Infinity", "Infinity", "NaN"}

// SanitizeNonFiniteTokens rewrites bare NaN / Infinity / -Infinity tokens to
// null so encoding/json accepts the line.
//
// The replacement is string-literal aware: a source path or model name
// containing the literal text "NaN" must survive untouched, so the scan
// tracks quoting and backslash escapes rather than doing a blind
// bytes.ReplaceAll.
//
// Mapping the tokens to null (rather than to a sentinel number) is what keeps
// the downstream semantics right: every reader here treats a null field as
// "absent", which is exactly what an unmeasured aggregate is. The eligibility
// filter already drops rows whose vmaf_score is absent or non-finite, so a
// NaN-scored row is skipped identically whether it arrived as NaN or as null.
func SanitizeNonFiniteTokens(line []byte) []byte {
	// Fast path: most rows carry no non-finite token at all.
	if !bytes.Contains(line, []byte("NaN")) &&
		!bytes.Contains(line, []byte("Infinity")) {
		return line
	}

	out := make([]byte, 0, len(line))
	inString := false
	for i := 0; i < len(line); {
		c := line[i]
		if inString {
			out = append(out, c)
			switch c {
			case '\\':
				// Copy the escaped byte verbatim so an escaped quote does not
				// end the string.
				if i+1 < len(line) {
					out = append(out, line[i+1])
					i += 2
					continue
				}
			case '"':
				inString = false
			}
			i++
			continue
		}
		if c == '"' {
			inString = true
			out = append(out, c)
			i++
			continue
		}
		matched := false
		for _, token := range nonFiniteTokens {
			if bytes.HasPrefix(line[i:], []byte(token)) {
				out = append(out, []byte("null")...)
				i += len(token)
				matched = true
				break
			}
		}
		if matched {
			continue
		}
		out = append(out, c)
		i++
	}
	return out
}

// trimSpace strips ASCII whitespace from both ends of b.
func trimSpace(b []byte) []byte {
	start := 0
	for start < len(b) && isSpace(b[start]) {
		start++
	}
	end := len(b)
	for end > start && isSpace(b[end-1]) {
		end--
	}
	return b[start:end]
}

// isSpace reports whether c is ASCII whitespace.
func isSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'
}

// SmallestPassingCRF returns (src, preset, crf, vmaf) for the highest-quality
// passing encode across a visited-row stream, grouped per (src, preset) and
// returning the first such pair in row order.
//
// This is the encode-driven path's picker (vmaftune.cli._smallest_passing_crf),
// distinct from PickTargetVMAF because it keys on (src, preset) rather than
// evaluating a flat row set.
func SmallestPassingCRF(rows []Row, targetVMAF float64) (src, preset string, crf int, vmaf float64, ok bool) {
	type key struct{ src, preset string }
	type best struct {
		crf   int
		score float64
	}
	bests := map[key]best{}

	for _, row := range rows {
		score, hasScore := numField(row, "vmaf_score")
		if !hasScore || score < targetVMAF {
			continue
		}
		rowSrc, _ := strField(row, "src")
		rowPreset, _ := strField(row, "preset")
		rowCRF, _ := intField(row, "crf")
		k := key{rowSrc, rowPreset}
		cur, seen := bests[k]
		// Smallest CRF that still meets the target is the highest quality at
		// acceptable cost; ties break on the higher score for determinism.
		if !seen || rowCRF < cur.crf || (rowCRF == cur.crf && score > cur.score) {
			bests[k] = best{rowCRF, score}
		}
	}
	if len(bests) == 0 {
		return "", "", 0, 0, false
	}
	for _, row := range rows {
		rowSrc, _ := strField(row, "src")
		rowPreset, _ := strField(row, "preset")
		if b, seen := bests[key{rowSrc, rowPreset}]; seen {
			return rowSrc, rowPreset, b.crf, b.score, true
		}
	}
	return "", "", 0, 0, false
}
