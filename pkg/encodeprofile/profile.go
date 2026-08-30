// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package encodeprofile is the Go port of vmaftune.encoder_profile plus the
// single-pass slice of vmaftune.encode that `vmaf-tune encode-profile`
// consumes.
//
// The subcommand reads the machine-readable `encoder_profile` payload every
// vmaf-tune report embeds, picks one recommendation out of it, and reproduces
// that exact encode with FFmpeg. The profile can arrive as report JSON, as the
// report HTML (payload inside a <pre> block) or as the report Markdown (payload
// inside a ```json fence), so loading has to handle all three.
//
// Scope: single-pass encodes only. EncodeRequest in Python also carries
// pass_number / stats_path for the Phase-F two-pass driver (ADR-0333), but
// `encode-profile` never sets them — the CLI has no --two-pass flag and always
// builds a pass_number=0 request. Porting the two-pass argv splice would mean
// porting every adapter's two_pass_args with no caller to exercise it, so it
// is deliberately left in Python until the `corpus` port needs it.
//
// ADR-0705 / ADR-0730 / ADR-0770: staged Go port of vmaf-tune.
package encodeprofile

import (
	"encoding/json"
	"errors"
	"fmt"
	"html"
	"math"
	"os"
	"regexp"
	"strings"
)

// SchemaID is report.ENCODER_PROFILE_SCHEMA — the only profile schema this
// loader accepts.
const SchemaID = "vmaftune.encoder_profile.v1"

// Profile is a decoded encoder_profile payload. It is kept as a raw map so
// unrecognised keys survive into the `selected` block of the command's JSON
// output byte-for-byte.
type Profile map[string]any

// Recommendation is one row of Profile["recommendations"].
type Recommendation map[string]any

var (
	// htmlPreRe matches the raw-JSON <pre> block a report HTML page carries.
	htmlPreRe = regexp.MustCompile(`(?is)<pre>(.*?)</pre>`)

	// mdJSONRe matches the fenced JSON payload in a report Markdown file.
	mdJSONRe = regexp.MustCompile("(?is)```json\\s*(.*?)\\s*```")
)

// rawVideoSuffixes are the extensions treated as raw (headerless) video, for
// which FFmpeg must be told the geometry explicitly.
var rawVideoSuffixes = map[string]struct{}{
	".raw": {}, ".yuv": {}, ".rgb": {}, ".gray": {},
}

// LoadProfilePayload reads an encoder profile from report JSON, report HTML or
// report Markdown.
func LoadProfilePayload(path string) (Profile, error) {
	b, err := os.ReadFile(path) // #nosec G304 -- path is a CLI flag, not remote input
	if err != nil {
		return nil, fmt.Errorf("cannot read profile %s: %w", path, err)
	}
	payload, err := jsonFromText(string(b), path)
	if err != nil {
		return nil, err
	}
	return extractProfile(payload)
}

// jsonFromText finds and decodes the JSON payload inside text. A document that
// already starts with '{' is parsed directly; otherwise the extension decides
// whether to look for an HTML <pre> block or a Markdown ```json fence.
func jsonFromText(text, source string) (map[string]any, error) {
	if strings.HasPrefix(strings.TrimLeft(text, " \t\r\n\v\f"), "{") {
		return decodeObject(text, source)
	}

	lower := strings.ToLower(source)
	if strings.HasSuffix(lower, ".html") || strings.HasSuffix(lower, ".htm") {
		m := htmlPreRe.FindStringSubmatch(text)
		if m == nil {
			return nil, errors.New("HTML report does not contain a raw JSON <pre> block")
		}
		return decodeObject(html.UnescapeString(m[1]), source)
	}

	m := mdJSONRe.FindStringSubmatch(text)
	if m == nil {
		return nil, errors.New("report does not contain a fenced JSON payload")
	}
	return decodeObject(m[1], source)
}

// decodeObject decodes a JSON object with UseNumber so numeric literals keep
// their int-vs-float identity through to the command's output.
func decodeObject(text, source string) (map[string]any, error) {
	dec := json.NewDecoder(strings.NewReader(text))
	dec.UseNumber()
	var out map[string]any
	if err := dec.Decode(&out); err != nil {
		return nil, fmt.Errorf("cannot parse profile JSON from %s: %w", source, err)
	}
	return out, nil
}

// extractProfile unwraps a full report payload down to its encoder_profile
// block and validates the schema.
func extractProfile(payload map[string]any) (Profile, error) {
	if nested, ok := payload["encoder_profile"].(map[string]any); ok {
		payload = nested
	}
	schema, _ := payload["schema"].(string)
	if schema != SchemaID {
		return nil, fmt.Errorf(
			"unsupported encoder profile schema %s; expected %q",
			pyRepr(payload["schema"]), SchemaID)
	}
	if _, ok := payload["recommendations"].([]any); !ok {
		return nil, errors.New("encoder profile has no recommendations list")
	}
	return payload, nil
}

// pyRepr renders a value the way CPython's %r would inside the schema-mismatch
// message, so the Go and Python error strings match.
func pyRepr(v any) string {
	switch t := v.(type) {
	case nil:
		return "None"
	case string:
		return "'" + strings.ReplaceAll(t, "'", "\\'") + "'"
	case bool:
		if t {
			return "True"
		}
		return "False"
	case json.Number:
		return t.String()
	default:
		return fmt.Sprint(v)
	}
}

// ---------------------------------------------------------------------------
// Recommendation selection
// ---------------------------------------------------------------------------

// SelectOptions narrows and indexes the candidate recommendations.
type SelectOptions struct {
	// Codec restricts the candidates to one codec. Empty means no filter.
	Codec string

	// TargetVMAF restricts the candidates to one target. Nil means no filter.
	TargetVMAF *float64

	// Index picks the Nth candidate (zero-based) after filtering. Nil takes
	// the first, which is the lowest-bitrate Pareto-selected row.
	Index *int
}

// SelectRecommendation picks one recommendation out of a profile.
//
// The default is the first Pareto-selected row with the lowest bitrate.
// Filters narrow the candidate set before the optional index is applied.
func SelectRecommendation(profile Profile, opts SelectOptions) (Recommendation, error) {
	rawRecs, _ := profile["recommendations"].([]any)

	recs := make([]Recommendation, 0, len(rawRecs))
	for _, r := range rawRecs {
		m, ok := r.(map[string]any)
		if !ok {
			continue
		}
		if opts.Codec != "" {
			codec, _ := m["codec"].(string)
			if codec != opts.Codec {
				continue
			}
		}
		if opts.TargetVMAF != nil {
			got := finiteFloatOr(m["target_vmaf"], math.NaN())
			if !isClose(got, *opts.TargetVMAF) {
				continue
			}
		}
		recs = append(recs, m)
	}
	if len(recs) == 0 {
		return nil, errors.New(
			"encoder profile has no recommendation matching the requested filters")
	}

	sortRecommendations(recs)

	if opts.Index != nil {
		i := *opts.Index
		if i < 0 || i >= len(recs) {
			return nil, fmt.Errorf(
				"recommendation index %d outside filtered range 0..%d", i, len(recs)-1)
		}
		return recs[i], nil
	}
	return recs[0], nil
}

// sortRecommendations orders candidates the way encoder_profile.select_
// recommendation does: Pareto-selected rows first, then ascending bitrate,
// then codec name, then target VMAF. Missing / non-finite numbers sort last
// via +Inf, matching the Python default.
//
// The sort is stable, matching CPython's list.sort, so rows tied on all four
// keys keep their profile order.
func sortRecommendations(recs []Recommendation) {
	type key struct {
		notPareto  int
		bitrate    float64
		codec      string
		targetVMAF float64
	}
	keyOf := func(r Recommendation) key {
		notPareto := 1
		if truthy(r["selected_pareto"]) {
			notPareto = 0
		}
		return key{
			notPareto:  notPareto,
			bitrate:    finiteFloatOr(r["bitrate_kbps"], math.Inf(1)),
			codec:      pyStrOr(r["codec"], ""),
			targetVMAF: finiteFloatOr(r["target_vmaf"], math.Inf(1)),
		}
	}
	stableSortBy(recs, func(a, b Recommendation) bool {
		ka, kb := keyOf(a), keyOf(b)
		if ka.notPareto != kb.notPareto {
			return ka.notPareto < kb.notPareto
		}
		if ka.bitrate != kb.bitrate {
			return ka.bitrate < kb.bitrate
		}
		if ka.codec != kb.codec {
			return ka.codec < kb.codec
		}
		return ka.targetVMAF < kb.targetVMAF
	})
}

// isClose is CPython's math.isclose with the abs_tol=1e-6 the selector passes
// and the default rel_tol=1e-09.
func isClose(a, b float64) bool {
	if math.IsNaN(a) || math.IsNaN(b) {
		return false
	}
	if a == b {
		return true
	}
	if math.IsInf(a, 0) || math.IsInf(b, 0) {
		return false
	}
	const relTol, absTol = 1e-09, 1e-6
	diff := math.Abs(a - b)
	return diff <= math.Max(relTol*math.Max(math.Abs(a), math.Abs(b)), absTol)
}

// ---------------------------------------------------------------------------
// EncodeRequest construction
// ---------------------------------------------------------------------------

// EncodeRequest is one (preset, quality) encode against one source — the
// single-pass subset of vmaftune.encode.EncodeRequest.
type EncodeRequest struct {
	Source    string
	Width     int
	Height    int
	PixFmt    string
	Framerate float64
	Encoder   string
	Preset    string
	CRF       int
	Output    string

	// ExtraParams are raw FFmpeg tokens appended after the codec args.
	ExtraParams []string

	// SampleClipSeconds opts into sample-clip mode (ADR-0297): FFmpeg's input
	// is sliced to an N-second window. Zero keeps the full-source encode.
	SampleClipSeconds float64

	// SampleClipStartS is the clip offset. The encode driver never recomputes
	// it, so the score driver can mirror the same window.
	SampleClipStartS float64

	// SourceIsContainer suppresses the raw-video input flags so FFmpeg
	// auto-detects the format.
	SourceIsContainer bool

	// DurationS bounds the encode to the analysed window when sample-clip
	// mode is off (ADR-0506).
	DurationS float64
}

// BuildOptions carries the CLI overrides that shape an EncodeRequest.
type BuildOptions struct {
	Output string

	// The *Override fields correspond one-to-one with the encode-profile
	// flags. A nil pointer means "not given"; the profile value is used.
	SourceOverride    string
	PresetOverride    string
	PixFmtOverride    string
	FramerateOverride *float64
	WidthOverride     *int
	HeightOverride    *int
	DurationOverride  *float64

	// SourceKind is "auto", "container" or "raw".
	SourceKind string

	SampleClipSeconds float64
	SampleClipStartS  float64
	ExtraParams       []string
}

// InferSourceIsContainer reports whether FFmpeg should auto-detect the source
// container.
func InferSourceIsContainer(source, sourceKind string) (bool, error) {
	switch sourceKind {
	case "container":
		return true, nil
	case "raw":
		return false, nil
	case "auto", "":
		_, isRaw := rawVideoSuffixes[strings.ToLower(pathSuffix(source))]
		return !isRaw, nil
	default:
		return false, fmt.Errorf("unknown source kind %q", sourceKind)
	}
}

// BuildEncodeRequest turns one selected profile row into an EncodeRequest.
func BuildEncodeRequest(
	profile Profile,
	rec Recommendation,
	opts BuildOptions,
) (EncodeRequest, error) {
	// Profiles written by older vmaf-tune versions store "source" as a plain
	// path string rather than a metadata dict; normalise before use.
	sourceMeta := map[string]any{}
	switch t := profile["source"].(type) {
	case map[string]any:
		sourceMeta = t
	case string:
		sourceMeta = map[string]any{"path": t}
	}
	runMeta, _ := profile["run"].(map[string]any)
	if runMeta == nil {
		runMeta = map[string]any{}
	}

	// Both branches go through pathString: argparse types --src as a Path, so
	// CPython normalises an overridden source exactly as it normalises a
	// stored one, and the normalised form is what lands in the argv.
	source := pathString(opts.SourceOverride)
	if opts.SourceOverride == "" {
		source = pathString(pyStrOr(sourceMeta["path"], ""))
	}
	if source == "" {
		return EncodeRequest{}, errors.New("profile has no source path; pass --src")
	}

	codec := pyStrOr(rec["codec"], "")
	if codec == "" {
		return EncodeRequest{}, errors.New("selected recommendation has no codec")
	}

	qualityVal, hasCRF := rec["crf"]
	if !hasCRF {
		qualityVal = rec["quality"]
		if qualityVal == nil {
			qualityVal = json.Number("-1")
		}
	}
	quality, ok := toInt(qualityVal)
	if !ok || quality < 0 {
		return EncodeRequest{}, errors.New("selected recommendation has no usable CRF/quality")
	}

	sourceIsContainer, err := InferSourceIsContainer(source, opts.SourceKind)
	if err != nil {
		return EncodeRequest{}, err
	}

	width := pickInt(opts.WidthOverride, sourceMeta["width"])
	height := pickInt(opts.HeightOverride, sourceMeta["height"])
	framerate := pickFloat(opts.FramerateOverride, sourceMeta["fps"])

	pixFmt := opts.PixFmtOverride
	if pixFmt == "" {
		pixFmt = pyStrOr(runMeta["pix_fmt"], "")
	}
	if pixFmt == "" {
		pixFmt = "yuv420p"
	}

	if !sourceIsContainer && (width <= 0 || height <= 0 || framerate <= 0) {
		return EncodeRequest{}, errors.New(
			"raw sources require width, height, and framerate in profile or flags")
	}

	preset := opts.PresetOverride
	if preset == "" {
		preset = pyStrOr(rec["preset"], "")
	}
	if preset == "" {
		preset = pyStrOr(runMeta["preset"], "")
	}
	if preset == "" || preset == "adapter default" {
		preset = DefaultPreset(codec)
	}

	var duration float64
	if opts.DurationOverride != nil {
		duration = *opts.DurationOverride
	} else {
		duration = finiteFloatOr(sourceMeta["duration_s"], 0)
		if math.IsNaN(duration) {
			duration = 0
		}
	}

	return EncodeRequest{
		Source:            source,
		Width:             width,
		Height:            height,
		PixFmt:            pixFmt,
		Framerate:         framerate,
		Encoder:           codec,
		Preset:            preset,
		CRF:               quality,
		Output:            pathString(opts.Output),
		ExtraParams:       opts.ExtraParams,
		SampleClipSeconds: opts.SampleClipSeconds,
		SampleClipStartS:  opts.SampleClipStartS,
		SourceIsContainer: sourceIsContainer,
		DurationS:         duration,
	}, nil
}

// pickInt implements CPython's `int(override or stored or 0)`: a zero override
// is falsy, so it falls through to the stored value exactly as Python does.
func pickInt(override *int, stored any) int {
	if override != nil && *override != 0 {
		return *override
	}
	v, ok := toInt(stored)
	if !ok {
		return 0
	}
	return v
}

// pickFloat implements CPython's `float(override or stored or 0.0)`.
func pickFloat(override *float64, stored any) float64 {
	if override != nil && *override != 0 {
		return *override
	}
	v, ok := toFloat(stored)
	if !ok || math.IsNaN(v) {
		return 0
	}
	return v
}
