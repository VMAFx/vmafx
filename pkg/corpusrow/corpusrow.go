// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package corpusrow ports the parts of tools/vmaf-tune/src/vmaftune/corpus.py
// that the `recommend` subcommand's encode-driven path needs: the schema-v3
// JSONL row, the coarse-to-fine CRF grid helpers, and the sweep loop that
// drives encode -> score per (preset, crf) cell.
//
// # Scope
//
// The `corpus` subcommand itself is a separate porting workstream. This
// package deliberately implements only the row shape and the coarse-to-fine
// driver, because `recommend --coarse-to-fine` writes the same JSONL and
// downstream tools read it. Five corpus features are NOT carried here, and
// their row fields are emitted at the same zero / empty values the Python
// emits when the feature is unavailable, so a reader can filter on them
// exactly as it already does:
//
//   - the content-addressed encode cache (ADR-0298)
//   - HDR detection and the HDR codec-arg injection (ADR-0295)
//   - TransNet-V2 shot metadata (research-0086) — shot_count stays 0
//   - sample-clip windowing (ADR-0301) — clip_mode stays "full"
//   - encoder-internal pass-1 stats (ADR-0332) — the ten enc_internal_*
//     columns stay 0.0, which is what the Python aggregator returns for an
//     empty frame list
//
// The canonical-6 feature aggregates ARE carried: pkg/scorecli parses them
// out of the libvmaf JSON, and a feature the run did not emit becomes NaN
// rather than a synthetic zero (ADR-0366).
package corpusrow

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/VMAFx/vmafx/pkg/ffencode"
	"github.com/VMAFx/vmafx/pkg/pyjson"
	"github.com/VMAFx/vmafx/pkg/scorecli"
)

// SchemaVersion is the corpus JSONL schema this package emits.
const SchemaVersion = 3

// Canonical6 is the feature order the mean / std columns follow. Positional
// consumers downstream depend on it staying stable.
var Canonical6 = []string{
	"adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2",
}

// encInternalKeys are the ADR-0332 encoder-stats columns. They are always
// emitted so schema-v3 rows are uniform across codecs.
var encInternalKeys = []string{
	"enc_internal_qp_mean", "enc_internal_qp_std",
	"enc_internal_bits_mean", "enc_internal_bits_std",
	"enc_internal_mv_mean", "enc_internal_mv_std",
	"enc_internal_itex_mean", "enc_internal_ptex_mean",
	"enc_internal_intra_ratio", "enc_internal_skip_ratio",
}

// Row is one corpus JSONL record.
type Row map[string]any

// Job describes the source under sweep.
type Job struct {
	Source    string
	Width     int
	Height    int
	PixFmt    string
	Framerate float64
	DurationS float64
	// SrcSHA256 is the source digest, empty when hashing is disabled.
	SrcSHA256 string
}

// Options describes how the sweep runs.
type Options struct {
	Encoder     string
	EncodeDir   string
	VMAFModel   string
	FFmpegBin   string
	VMAFBin     string
	KeepEncodes bool
	// ScoreBackend is forwarded to the libvmaf CLI as --backend. Empty lets
	// the binary pick its own default.
	ScoreBackend string
}

// newRunID returns a 32-hex-character run identifier, matching the Python
// uuid4().hex shape.
func newRunID() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		// A failed CSPRNG read is not worth aborting a sweep over: the run
		// id is a correlation handle, not a security token. Fall back to a
		// timestamp-derived value that is still unique per row in practice.
		return fmt.Sprintf("%032x", time.Now().UnixNano())
	}
	return hex.EncodeToString(b[:])
}

// NewRow builds a schema-v3 row from one encode plus its score.
func NewRow(job Job, opts Options, preset string, crf int,
	enc ffencode.Result, score scorecli.Result, scoreModel string,
) Row {
	encodedDuration := job.DurationS
	if encodedDuration <= 0 {
		encodedDuration = 1.0
	}
	encodePath := ""
	if opts.KeepEncodes {
		encodePath = enc.Request.Output
	}
	extraParams := enc.Request.ExtraParams
	if extraParams == nil {
		extraParams = []string{}
	}
	exitStatus := enc.ExitStatus
	if exitStatus == 0 {
		exitStatus = score.ExitStatus
	}

	row := Row{
		"schema_version":      SchemaVersion,
		"run_id":              newRunID(),
		"timestamp":           time.Now().UTC().Format(time.RFC3339),
		"src":                 job.Source,
		"src_sha256":          job.SrcSHA256,
		"width":               job.Width,
		"height":              job.Height,
		"pix_fmt":             job.PixFmt,
		"framerate":           job.Framerate,
		"duration_s":          job.DurationS,
		"encoder":             opts.Encoder,
		"encoder_version":     enc.EncoderVersion,
		"preset":              preset,
		"crf":                 crf,
		"extra_params":        extraParams,
		"encode_path":         encodePath,
		"encode_size_bytes":   enc.EncodeSizeBytes,
		"bitrate_kbps":        ffencode.BitrateKbps(enc.EncodeSizeBytes, encodedDuration),
		"encode_time_ms":      enc.EncodeTimeMS,
		"vmaf_score":          score.VMAFScore,
		"vmaf_model":          scoreModel,
		"score_time_ms":       score.ScoreTimeMS,
		"ffmpeg_version":      enc.FFmpegVersion,
		"vmaf_binary_version": score.VMAFBinaryVersion,
		"exit_status":         exitStatus,
		// Corpus-group-only features; see the package doc comment.
		"clip_mode":             "full",
		"hdr_transfer":          "",
		"hdr_primaries":         "",
		"hdr_forced":            false,
		"shot_count":            0,
		"shot_avg_duration_sec": 0.0,
		"shot_duration_std_sec": 0.0,
	}
	for _, feature := range Canonical6 {
		mean, ok := score.FeatureMeans[feature]
		if !ok {
			mean = math.NaN()
		}
		std, hasStd := score.FeatureStds[feature]
		if !hasStd {
			std = math.NaN()
		}
		row[feature+"_mean"] = mean
		row[feature+"_std"] = std
	}
	for _, key := range encInternalKeys {
		row[key] = 0.0
	}
	return row
}

// Keys returns the schema-v3 key set, sorted, for schema-shape assertions.
func Keys() []string {
	probe := NewRow(Job{}, Options{}, "", 0, ffencode.Result{}, scorecli.Result{}, "")
	out := make([]string, 0, len(probe))
	for k := range probe {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// CRFClamp bounds a CRF candidate to the libx264 0..51 valid range.
func CRFClamp(crf int) int {
	switch {
	case crf < 0:
		return 0
	case crf > 51:
		return 51
	default:
		return crf
	}
}

// CoarseGridCRFs returns the coarse-pass CRF grid, deduped and sorted.
//
// The defaults yield 10, 20, 30, 40, 50 — five points spanning the
// practically useful range for libx264. CRF below 10 is visually lossless on
// most content (huge bitrate, no perceptual gain) and 51 is the codec floor,
// so the coarse pass skips both.
func CoarseGridCRFs(crfMin, crfMax, coarseStep int) ([]int, error) {
	if coarseStep <= 0 {
		return nil, fmt.Errorf("coarse_step must be positive, got %d", coarseStep)
	}
	if crfMin > crfMax {
		return nil, fmt.Errorf("crf_min (%d) > crf_max (%d)", crfMin, crfMax)
	}
	n := (crfMax-crfMin)/coarseStep + 1
	seen := make(map[int]bool, n)
	grid := make([]int, 0, n)
	for i := 0; i < n; i++ {
		c := CRFClamp(crfMin + i*coarseStep)
		if !seen[c] {
			seen[c] = true
			grid = append(grid, c)
		}
	}
	sort.Ints(grid)
	return grid, nil
}

// FineGridCRFs returns the CRF candidates within fineRadius of bestCRF at
// fineStep, minus the already-measured exclude set (typically the coarse
// grid) and outside [crfMin, crfMax].
func FineGridCRFs(bestCRF, fineRadius, fineStep, crfMin, crfMax int, exclude []int) ([]int, error) {
	if fineRadius < 0 {
		return nil, fmt.Errorf("fine_radius must be non-negative, got %d", fineRadius)
	}
	if fineStep <= 0 {
		return nil, fmt.Errorf("fine_step must be positive, got %d", fineStep)
	}
	excluded := make(map[int]bool, len(exclude))
	for _, c := range exclude {
		excluded[c] = true
	}
	seen := map[int]bool{}
	out := make([]int, 0, 2*fineRadius/fineStep+1)
	for delta := -fineRadius; delta <= fineRadius; delta += fineStep {
		c := CRFClamp(bestCRF + delta)
		if excluded[c] || seen[c] || c < crfMin || c > crfMax {
			continue
		}
		seen[c] = true
		out = append(out, c)
	}
	sort.Ints(out)
	return out, nil
}

// rowScore reads a row's VMAF score, returning NaN when it is missing or
// unparseable.
func rowScore(row Row) float64 {
	v, ok := row["vmaf_score"]
	if !ok {
		return math.NaN()
	}
	f, ok := v.(float64)
	if !ok {
		return math.NaN()
	}
	return f
}

// rowCRF reads a row's CRF.
func rowCRF(row Row) int {
	switch v := row["crf"].(type) {
	case int:
		return v
	case float64:
		return int(v)
	default:
		return 0
	}
}

// PickBestCRF identifies the coarse CRF to refine around.
//
// With a target: the HIGHEST CRF whose score meets it — the
// smallest-quality candidate that still passes the gate, so refining around
// it locates the smallest acceptable CRF. When nothing meets the target, the
// highest-VMAF coarse point, so the fine pass at least probes near the
// achievable ceiling.
//
// Without a target: the CRF with the highest VMAF. Rows with a NaN score are
// ignored throughout.
func PickBestCRF(rows []Row, targetVMAF *float64) (int, bool) {
	var valid []Row
	for _, r := range rows {
		if !math.IsNaN(rowScore(r)) {
			valid = append(valid, r)
		}
	}
	if len(valid) == 0 {
		return 0, false
	}

	bestByScore := func(candidates []Row) int {
		winner := candidates[0]
		for _, r := range candidates[1:] {
			if rowScore(r) > rowScore(winner) {
				winner = r
			}
		}
		return rowCRF(winner)
	}

	if targetVMAF == nil {
		return bestByScore(valid), true
	}
	var passing []Row
	for _, r := range valid {
		if rowScore(r) >= *targetVMAF {
			passing = append(passing, r)
		}
	}
	if len(passing) == 0 {
		return bestByScore(valid), true
	}
	winner := passing[0]
	for _, r := range passing[1:] {
		if rowCRF(r) > rowCRF(winner) {
			winner = r
		}
	}
	return rowCRF(winner), true
}

// ShouldSkipRefinement decides whether the coarse pass alone is enough.
//
// The fine pass is skipped when the coarse pass produced no measurable rows,
// or when a target is set, the best-coarse CRF already meets it, and refining
// higher cannot help — the best-coarse is already at the top of the coarse
// grid or pinned at crfMax, so there are no larger CRF candidates to probe
// and the existing best already minimises bitrate at the gate.
func ShouldSkipRefinement(bestCRF int, haveBest bool, coarseGrid []int,
	targetVMAF *float64, bestScore float64, crfMax int,
) bool {
	if !haveBest {
		return true
	}
	if targetVMAF == nil {
		return false
	}
	if math.IsNaN(bestScore) {
		return false
	}
	if bestScore < *targetVMAF {
		return false
	}
	maxCoarse := 0
	for _, c := range coarseGrid {
		if c > maxCoarse {
			maxCoarse = c
		}
	}
	return bestCRF >= maxCoarse || bestCRF >= crfMax
}

// CellRunner encodes and scores one (preset, crf) cell and returns its row.
// The sweep injects the production implementation; tests inject a stub.
type CellRunner func(ctx context.Context, preset string, crf int) (Row, error)

// SearchOptions configures CoarseToFineSearch.
type SearchOptions struct {
	Presets    []string
	TargetVMAF *float64
	CoarseStep int
	FineRadius int
	FineStep   int
	CRFMin     int
	CRFMax     int
}

// DefaultSearchOptions returns the Python defaults: a 5-point coarse grid
// over 10..50 and a +/-5 fine pass at step 1.
func DefaultSearchOptions() SearchOptions {
	return SearchOptions{
		CoarseStep: 10, FineRadius: 5, FineStep: 1, CRFMin: 10, CRFMax: 50,
	}
}

// CoarseToFineSearch runs the two-pass CRF search, returning every visited
// row in coarse-then-fine order per preset.
//
// The caller selects the winning CRF from the rows; this function only drives
// the encodes. With the defaults that is 5 coarse plus up to 10 fine cells —
// 15 encodes against 52 for a full 0..51 sweep (ADR-0296).
func CoarseToFineSearch(ctx context.Context, run CellRunner, opts SearchOptions) ([]Row, error) {
	if len(opts.Presets) == 0 {
		return nil, nil
	}
	coarseGrid, err := CoarseGridCRFs(opts.CRFMin, opts.CRFMax, opts.CoarseStep)
	if err != nil {
		return nil, err
	}

	var visited []Row
	seenPreset := map[string]bool{}
	for _, preset := range opts.Presets {
		if seenPreset[preset] {
			continue
		}
		seenPreset[preset] = true

		var coarseRows []Row
		for _, crf := range coarseGrid {
			row, cellErr := run(ctx, preset, crf)
			if cellErr != nil {
				return nil, cellErr
			}
			coarseRows = append(coarseRows, row)
			visited = append(visited, row)
		}

		bestCRF, haveBest := PickBestCRF(coarseRows, opts.TargetVMAF)
		bestScore := math.NaN()
		if haveBest {
			for _, r := range coarseRows {
				if rowCRF(r) == bestCRF {
					bestScore = rowScore(r)
					break
				}
			}
		}
		if ShouldSkipRefinement(
			bestCRF, haveBest, coarseGrid, opts.TargetVMAF, bestScore, opts.CRFMax) {
			continue
		}

		fineCRFs, fineErr := FineGridCRFs(
			bestCRF, opts.FineRadius, opts.FineStep, opts.CRFMin, opts.CRFMax, coarseGrid)
		if fineErr != nil {
			return nil, fineErr
		}
		for _, crf := range fineCRFs {
			row, cellErr := run(ctx, preset, crf)
			if cellErr != nil {
				return nil, cellErr
			}
			visited = append(visited, row)
		}
	}
	return visited, nil
}

// MarshalRow renders one row as a single-line JSON object with sorted keys.
//
// It does NOT use encoding/json for the row itself, for one load-bearing
// reason: a feature aggregate the run did not measure is NaN by design
// (ADR-0366 — absence must not become a synthetic zero the trainers learn
// from), and encoding/json refuses to marshal NaN at all.
//
// Python's json.dumps writes bare NaN / Infinity / -Infinity tokens. They are
// not standard JSON, but json.loads and pandas both read them back, and the
// corpus JSONL's consumers are exactly those Python trainers. Emitting null
// instead would turn float('nan') into None downstream, where float(None)
// raises rather than propagating a NaN.
//
// So this writer reproduces Python's output exactly — the same non-finite
// tokens, json.dumps's default ", " / ": " separators, and repr's float
// rendering — via pkg/pyjson, which is the one implementation of that
// contract in the tree (ADR-1137).
func MarshalRow(row Row) (string, error) {
	blob, err := pyjson.Marshal(map[string]any(row), pyjson.Options{SortKeys: true})
	if err != nil {
		return "", fmt.Errorf("corpusrow: marshal row: %w", err)
	}
	return string(blob), nil
}

// WriteJSONL writes rows to path, one JSON object per line with sorted keys
// (matching the Python writer's sort_keys=True).
func WriteJSONL(rows []Row, path string) (int, error) {
	// G301: 0o750 keeps the corpus directory owner+group readable only.
	if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
		return 0, fmt.Errorf("corpusrow: create output dir: %w", err)
	}
	var sb strings.Builder
	for _, row := range rows {
		line, err := MarshalRow(row)
		if err != nil {
			return 0, err
		}
		sb.WriteString(line)
		sb.WriteString("\n")
	}
	// G306: 0o600 — corpus rows carry source paths that leak dataset names.
	if err := os.WriteFile(path, []byte(sb.String()), 0o600); err != nil {
		return 0, fmt.Errorf("corpusrow: write %q: %w", path, err)
	}
	return len(rows), nil
}
