// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package report renders vmafx-tune compare results as Markdown or JSON.
//
// Stage 1 ships Markdown and JSON; HTML rendering is Stage-2 scope.
//
// Schema compatibility: the JSON shape mirrors the Python vmaf-tune
// schema-v1 (single-target) and schema-v2 (multi-target sweep) payloads
// in tools/vmaf-tune/src/vmaftune/compare.py so the existing report
// renderer (tools/vmaf-tune/src/vmaftune/report.py) can ingest the Go
// output unchanged.
package report

import (
	"encoding/json"
	"fmt"
	"math"
	"strings"

	"github.com/VMAFx/vmafx/pkg/bisect"
)

// ToolVersion is the version string stamped into every report.
// Overridden at build time via -ldflags for release binaries.
var ToolVersion = "dev"

// Row is one codec's best (CRF, bitrate, VMAF) result at a given target.
// Fields match the Python RecommendResult.to_row() output exactly so the
// existing vmaf-tune report.py renderer can ingest the JSON unchanged.
type Row struct {
	Codec          string  `json:"codec"`
	Adapter        string  `json:"adapter"`
	RuntimeVariant string  `json:"runtime_variant"`
	FFmpegBin      string  `json:"ffmpeg_bin"`
	EncoderVersion string  `json:"encoder_version"`
	BestCRF        int     `json:"best_crf"`
	BitratekBps    float64 `json:"bitrate_kbps"` // null-serialized when NaN
	EncodeTimeMS   float64 `json:"encode_time_ms"`
	VMAFScore      float64 `json:"vmaf_score"`
	TargetVMAF     float64 `json:"target_vmaf"`
	OK             bool    `json:"ok"`
	Error          string  `json:"error"`
	// BisectSamples is emitted only when non-empty (ADR-0530 additive field).
	BisectSamples []bisect.Sample `json:"bisect_samples,omitempty"`
}

// nanToNull replaces NaN and Inf float64 values with nil so JSON
// serialisation never produces bare NaN tokens (not valid RFC 8259).
// Mirrors vmaftune.compare._nan_to_none.
func nanToNull(v float64) any {
	if math.IsNaN(v) || math.IsInf(v, 0) {
		return nil
	}
	return v
}

// wireSample is the JSON wire shape for a bisect.Sample with non-finite
// floats coerced to JSON null. The shape mirrors bisect.Sample's JSON tags
// so the Python report.py renderer ingests it unchanged.
//
// AGENTS.md rebase-sensitive invariant #2 — the Go emitter must never write
// bare NaN / Infinity tokens. The plain bisect.Sample struct has
// float64 (not any) fields, so a single corrupt vmaf-XML mean= value (e.g.
// "NaN" — a value strconv.ParseFloat accepts without error) would propagate
// into Sample.VMAFScore and crash json.MarshalIndent later in the run with
// "json: unsupported value: NaN". sanitizeBisectSamples walks the slice and
// returns a wire shape with non-finite floats coerced to null.
type wireSample struct {
	CRF          int `json:"crf"`
	BitratekBps  any `json:"bitrate_kbps"`
	VMAFScore    any `json:"vmaf_score"`
	EncodeTimeMS any `json:"encode_time_ms"`
}

// SanitizeBisectSamples converts a slice of bisect.Sample to a slice of
// wireSample with non-finite floats (NaN / +Inf / -Inf) coerced to JSON
// null. Returns nil for an empty input so the emitter omits the
// bisect_samples key entirely (matching the omitempty JSON tag).
//
// Exported so cmd/vmafx-tune/cmd.emitSweepJSON can reuse the same logic for
// schema-v2 multi-target sweep payloads.
func SanitizeBisectSamples(samples []bisect.Sample) []any {
	if len(samples) == 0 {
		return nil
	}
	out := make([]any, len(samples))
	for i, s := range samples {
		out[i] = wireSample{
			CRF:          s.CRF,
			BitratekBps:  nanToNull(s.BitratekBps),
			VMAFScore:    nanToNull(s.VMAFScore),
			EncodeTimeMS: nanToNull(s.EncodeTimeMS),
		}
	}
	return out
}

// Report is the result of a compare run.
type Report struct {
	Src         string  `json:"src"`
	TargetVMAF  float64 `json:"target_vmaf"`
	ToolVersion string  `json:"tool_version"`
	WallTimeMS  float64 `json:"wall_time_ms"`
	Rows        []Row   `json:"rows"`
}

// Best returns the smallest-bitrate successful row, or nil when all rows
// failed.
func (r *Report) Best() *Row {
	for i := range r.Rows {
		if r.Rows[i].OK {
			return &r.Rows[i]
		}
	}
	return nil
}

// EmitJSON renders the report as pretty-printed JSON. NaN / Inf float values
// are coerced to null before serialisation (RFC 8259 compliance). The
// sanitisation walks into bisect_samples — a single NaN there is enough to
// crash json.MarshalIndent with "json: unsupported value: NaN" and break
// the Python ↔ Go parser-parity invariant (AGENTS.md #2).
func EmitJSON(report Report) (string, error) {
	// Sanitize non-finite floats before serialisation.
	type wireRow struct {
		Codec          string  `json:"codec"`
		Adapter        string  `json:"adapter"`
		RuntimeVariant string  `json:"runtime_variant"`
		FFmpegBin      string  `json:"ffmpeg_bin"`
		EncoderVersion string  `json:"encoder_version"`
		BestCRF        int     `json:"best_crf"`
		BitratekBps    any     `json:"bitrate_kbps"`
		EncodeTimeMS   any     `json:"encode_time_ms"`
		VMAFScore      any     `json:"vmaf_score"`
		TargetVMAF     float64 `json:"target_vmaf"`
		OK             bool    `json:"ok"`
		Error          string  `json:"error"`
		BisectSamples  []any   `json:"bisect_samples,omitempty"`
	}
	wireRows := make([]wireRow, len(report.Rows))
	for i, row := range report.Rows {
		wireRows[i] = wireRow{
			Codec:          row.Codec,
			Adapter:        row.Adapter,
			RuntimeVariant: row.RuntimeVariant,
			FFmpegBin:      row.FFmpegBin,
			EncoderVersion: row.EncoderVersion,
			BestCRF:        row.BestCRF,
			BitratekBps:    nanToNull(row.BitratekBps),
			EncodeTimeMS:   nanToNull(row.EncodeTimeMS),
			VMAFScore:      nanToNull(row.VMAFScore),
			TargetVMAF:     row.TargetVMAF,
			OK:             row.OK,
			Error:          row.Error,
			BisectSamples:  SanitizeBisectSamples(row.BisectSamples),
		}
	}
	payload := struct {
		Src         string    `json:"src"`
		TargetVMAF  float64   `json:"target_vmaf"`
		ToolVersion string    `json:"tool_version"`
		WallTimeMS  float64   `json:"wall_time_ms"`
		Rows        []wireRow `json:"rows"`
	}{
		Src:         report.Src,
		TargetVMAF:  report.TargetVMAF,
		ToolVersion: report.ToolVersion,
		WallTimeMS:  report.WallTimeMS,
		Rows:        wireRows,
	}
	b, err := json.MarshalIndent(payload, "", "  ")
	if err != nil {
		return "", fmt.Errorf("marshal report: %w", err)
	}
	return string(b) + "\n", nil
}

// EmitMarkdown renders the report as a Markdown table. The format mirrors
// tools/vmaf-tune/src/vmaftune/compare.py _emit_markdown.
func EmitMarkdown(report Report) string {
	var sb strings.Builder
	fmt.Fprintf(&sb, "# Codec comparison — target VMAF %g\n\n", report.TargetVMAF)
	fmt.Fprintf(&sb, "- Source: `%s`\n", report.Src)
	fmt.Fprintf(&sb, "- Tool: `vmafx-tune %s`\n", report.ToolVersion)
	fmt.Fprintf(&sb, "- Wall time: %.1f ms\n\n", report.WallTimeMS)
	sb.WriteString("| Rank | Codec | Encoder | Best CRF | Bitrate (kbps) | " +
		"Encode time (ms) | VMAF | Status |\n")
	sb.WriteString("|---:|---|---|---:|---:|---:|---:|---|\n")

	rank := 0
	for _, row := range report.Rows {
		var rankCell, status string
		if row.OK {
			rank++
			rankCell = fmt.Sprintf("%d", rank)
			status = "ok"
		} else {
			rankCell = "—"
			status = fmt.Sprintf("fail: %s", row.Error)
		}
		crfStr := "—"
		if row.BestCRF >= 0 {
			crfStr = fmt.Sprintf("%d", row.BestCRF)
		}
		bitrateStr := "—"
		if !math.IsNaN(row.BitratekBps) && row.BitratekBps > 0 {
			bitrateStr = fmt.Sprintf("%.1f", row.BitratekBps)
		}
		encTimeStr := "—"
		if !math.IsNaN(row.EncodeTimeMS) && row.EncodeTimeMS >= 0 {
			encTimeStr = fmt.Sprintf("%.1f", row.EncodeTimeMS)
		}
		vmafStr := "—"
		if !math.IsNaN(row.VMAFScore) && row.VMAFScore > 0 {
			vmafStr = fmt.Sprintf("%.2f", row.VMAFScore)
		}
		encoder := row.EncoderVersion
		if encoder == "" {
			encoder = "—"
		}
		fmt.Fprintf(&sb, "| %s | %s | %s | %s | %s | %s | %s | %s |\n",
			rankCell, row.Codec, encoder, crfStr, bitrateStr, encTimeStr, vmafStr, status)
	}

	best := report.Best()
	if best != nil {
		fmt.Fprintf(&sb, "\n**Smallest file**: `%s` at CRF %d → %.1f kbps (VMAF %.2f).\n",
			best.Codec, best.BestCRF, best.BitratekBps, best.VMAFScore)
	} else {
		sb.WriteString("\n**No codec succeeded.** See per-row error column.\n")
	}

	return sb.String()
}
