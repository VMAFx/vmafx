// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// This file extends package report with the multi-file rendering surface
// introduced in Stage 4 (ADR-0770).
//
// The public types here are intentionally kept separate from the Stage-1
// report.go so that the wire schema (Row, Report) is not entangled with the
// Stage-4 multi-file rendering path, and so that the schema-forward invariant
// noted in cmd/vmafx-tune/AGENTS.md remains easy to audit.
package report

import (
	"fmt"
	"html"
	"math"
	"sort"
	"strings"
)

// ---- Wire schemas (unmarshal targets for compare + ladder JSON outputs) ----

// CompareWirePayload is the JSON shape emitted by "vmafx-tune-go compare".
// Fields mirror EmitJSON's anonymous payload struct so json.Unmarshal round-
// trips cleanly.
type CompareWirePayload struct {
	Src         string    `json:"src"`
	TargetVMAF  float64   `json:"target_vmaf"`
	ToolVersion string    `json:"tool_version"`
	WallTimeMS  float64   `json:"wall_time_ms"`
	Rows        []WireRow `json:"rows"`
}

// WireRow is the row shape used in CompareWirePayload.  float64 fields that
// may be JSON null are decoded as *float64 so callers can distinguish 0 from
// null.
type WireRow struct {
	Codec          string   `json:"codec"`
	Adapter        string   `json:"adapter"`
	RuntimeVariant string   `json:"runtime_variant"`
	FFmpegBin      string   `json:"ffmpeg_bin"`
	EncoderVersion string   `json:"encoder_version"`
	BestCRF        int      `json:"best_crf"`
	BitratekBps    *float64 `json:"bitrate_kbps"`
	EncodeTimeMS   *float64 `json:"encode_time_ms"`
	VMAFScore      *float64 `json:"vmaf_score"`
	TargetVMAF     float64  `json:"target_vmaf"`
	OK             bool     `json:"ok"`
	Error          string   `json:"error"`
}

// LadderRendition is one selected rendition from the ladder JSON output.
type LadderRendition struct {
	Width       int     `json:"width"`
	Height      int     `json:"height"`
	BitratekBps float64 `json:"bitrate_kbps"`
	VMAF        float64 `json:"vmaf"`
	CRF         int     `json:"crf"`
}

// LadderWirePayload is the JSON shape emitted by "vmafx-tune-go ladder".
type LadderWirePayload struct {
	SchemaVersion int               `json:"schema_version"`
	Src           string            `json:"src"`
	Encoder       string            `json:"encoder"`
	TargetVMAFs   []float64         `json:"target_vmafs"`
	Resolutions   []string          `json:"resolutions"`
	ToolVersion   string            `json:"tool_version"`
	WallTimeMS    float64           `json:"wall_time_ms"`
	Renditions    []LadderRendition `json:"renditions"`
}

// ---- MultiReport — a normalised, schema-neutral in-memory representation ----

// ReportKind identifies whether a MultiReport came from a compare or ladder run.
type ReportKind string

const (
	// KindCompare is a MultiReport from a "compare" run.
	KindCompare ReportKind = "compare"
	// KindLadder is a MultiReport from a "ladder" run.
	KindLadder ReportKind = "ladder"
)

// MultiReport is the normalised, renderer-neutral representation of one input
// file.  Both compare and ladder payloads are converted to this shape so the
// Markdown / HTML renderers need only one code path.
type MultiReport struct {
	// Kind identifies the source schema.
	Kind ReportKind
	// InputPath is the file path that was loaded.
	InputPath string
	// Src is the source video path from the payload.
	Src string
	// ToolVersion is the tool_version field from the payload.
	ToolVersion string
	// WallTimeMS is the wall_time_ms field from the payload.
	WallTimeMS float64

	// CompareRows is populated only when Kind == KindCompare.
	CompareRows []WireRow
	// TargetVMAF is the single target from a v1 compare payload.
	TargetVMAF float64

	// LadderRenditions is populated only when Kind == KindLadder.
	LadderRenditions []LadderRendition
	// LadderEncoder is the encoder name from a ladder payload.
	LadderEncoder string
	// LadderTargetVMAFs are the target VMAF values from a ladder payload.
	LadderTargetVMAFs []float64
	// LadderResolutions are the resolution strings from a ladder payload.
	LadderResolutions []string
}

// MultiReportFromCompare converts a CompareWirePayload into a MultiReport.
func MultiReportFromCompare(p CompareWirePayload, inputPath string) MultiReport {
	return MultiReport{
		Kind:        KindCompare,
		InputPath:   inputPath,
		Src:         p.Src,
		ToolVersion: p.ToolVersion,
		WallTimeMS:  p.WallTimeMS,
		CompareRows: p.Rows,
		TargetVMAF:  p.TargetVMAF,
	}
}

// MultiReportFromLadder converts a LadderWirePayload into a MultiReport.
func MultiReportFromLadder(p LadderWirePayload, inputPath string) MultiReport {
	return MultiReport{
		Kind:              KindLadder,
		InputPath:         inputPath,
		Src:               p.Src,
		ToolVersion:       p.ToolVersion,
		WallTimeMS:        p.WallTimeMS,
		LadderRenditions:  p.Renditions,
		LadderEncoder:     p.Encoder,
		LadderTargetVMAFs: p.TargetVMAFs,
		LadderResolutions: p.Resolutions,
	}
}

// ---- Markdown renderer ----

// RenderMarkdownMulti renders a slice of MultiReports as GitHub-flavoured
// Markdown.  Reports are separated by horizontal rules and a per-file heading.
func RenderMarkdownMulti(reports []MultiReport) string {
	var sb strings.Builder
	sb.WriteString("# vmafx-tune report\n\n")
	sb.WriteString(fmt.Sprintf("_Generated by `vmafx-tune-go %s`_\n\n", ToolVersion))
	sb.WriteString(fmt.Sprintf("%d input(s)\n\n", len(reports)))

	for i, mr := range reports {
		if i > 0 {
			sb.WriteString("\n---\n\n")
		}
		switch mr.Kind {
		case KindCompare:
			renderCompareMarkdown(&sb, mr)
		case KindLadder:
			renderLadderMarkdown(&sb, mr)
		}
	}
	return sb.String()
}

func renderCompareMarkdown(sb *strings.Builder, mr MultiReport) {
	fmt.Fprintf(sb, "## Compare — `%s`\n\n", mr.Src)
	fmt.Fprintf(sb, "- Input: `%s`\n", mr.InputPath)
	fmt.Fprintf(sb, "- Tool: `%s`\n", mr.ToolVersion)
	fmt.Fprintf(sb, "- Target VMAF: %g\n", mr.TargetVMAF)
	fmt.Fprintf(sb, "- Wall time: %.1f ms\n\n", mr.WallTimeMS)

	sb.WriteString("| Rank | Codec | Encoder | Best CRF | Bitrate (kbps) | Encode time (ms) | VMAF | Status |\n")
	sb.WriteString("|---:|---|---|---:|---:|---:|---:|---|\n")

	// Sort: successful rows first by bitrate ascending, then failed rows.
	sorted := make([]WireRow, len(mr.CompareRows))
	copy(sorted, mr.CompareRows)
	sort.SliceStable(sorted, func(i, j int) bool {
		oki, okj := sorted[i].OK, sorted[j].OK
		if oki != okj {
			return oki // successful rows first
		}
		if oki {
			bi := float64PtrOr(sorted[i].BitratekBps, math.MaxFloat64)
			bj := float64PtrOr(sorted[j].BitratekBps, math.MaxFloat64)
			return bi < bj
		}
		return false
	})

	rank := 0
	for _, row := range sorted {
		var rankCell, status string
		if row.OK {
			rank++
			rankCell = fmt.Sprintf("%d", rank)
			status = "ok"
		} else {
			rankCell = "—"
			status = "fail: " + row.Error
		}
		crfStr := "—"
		if row.BestCRF >= 0 {
			crfStr = fmt.Sprintf("%d", row.BestCRF)
		}
		bitrateStr := fmtFloat(row.BitratekBps, "%.1f")
		encTimeStr := fmtFloat(row.EncodeTimeMS, "%.1f")
		vmafStr := fmtFloat(row.VMAFScore, "%.2f")
		encoder := row.EncoderVersion
		if encoder == "" {
			encoder = "—"
		}
		fmt.Fprintf(sb, "| %s | %s | %s | %s | %s | %s | %s | %s |\n",
			rankCell, row.Codec, encoder, crfStr, bitrateStr, encTimeStr, vmafStr, status)
	}
	sb.WriteByte('\n')
}

func renderLadderMarkdown(sb *strings.Builder, mr MultiReport) {
	fmt.Fprintf(sb, "## Ladder — `%s`\n\n", mr.Src)
	fmt.Fprintf(sb, "- Input: `%s`\n", mr.InputPath)
	fmt.Fprintf(sb, "- Tool: `%s`\n", mr.ToolVersion)
	fmt.Fprintf(sb, "- Encoder: `%s`\n", mr.LadderEncoder)
	if len(mr.LadderTargetVMAFs) > 0 {
		strs := make([]string, len(mr.LadderTargetVMAFs))
		for i, v := range mr.LadderTargetVMAFs {
			strs[i] = fmt.Sprintf("%g", v)
		}
		fmt.Fprintf(sb, "- VMAF targets: %s\n", strings.Join(strs, ", "))
	}
	if len(mr.LadderResolutions) > 0 {
		fmt.Fprintf(sb, "- Resolutions: %s\n", strings.Join(mr.LadderResolutions, ", "))
	}
	fmt.Fprintf(sb, "- Wall time: %.1f ms\n\n", mr.WallTimeMS)

	if len(mr.LadderRenditions) == 0 {
		sb.WriteString("_No renditions selected._\n\n")
		return
	}

	sb.WriteString("| Rank | Resolution | Bitrate (kbps) | VMAF | CRF |\n")
	sb.WriteString("|---:|---|---:|---:|---:|\n")
	for i, r := range mr.LadderRenditions {
		fmt.Fprintf(sb, "| %d | %dx%d | %.1f | %.2f | %d |\n",
			i+1, r.Width, r.Height, r.BitratekBps, r.VMAF, r.CRF)
	}
	sb.WriteByte('\n')
}

// ---- HTML renderer ----

// htmlPageTemplate is a minimal self-contained HTML5 page.  %s slots are:
// title, style (inlined), body HTML.
const htmlPageTemplate = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>%s</title>
<style>
%s
</style>
</head>
<body>
%s
</body>
</html>
`

const htmlCSS = `body{font-family:system-ui,sans-serif;margin:2rem auto;max-width:960px;color:#1a1a1a}
h1,h2{border-bottom:1px solid #ddd;padding-bottom:.25rem}
table{border-collapse:collapse;width:100%%;margin-bottom:1rem}
th,td{border:1px solid #ccc;padding:.4rem .6rem;text-align:left}
th{background:#f5f5f5}
tr:nth-child(even){background:#fafafa}
.ok{color:#2a7a2a;font-weight:bold}
.fail{color:#9a1a1a}
.meta{color:#555;font-size:.9em}
`

// RenderHTMLMulti renders a slice of MultiReports as a self-contained HTML page.
func RenderHTMLMulti(reports []MultiReport) string {
	var body strings.Builder

	fmt.Fprintf(&body, "<h1>vmafx-tune report</h1>\n")
	fmt.Fprintf(&body, "<p class=\"meta\">Generated by <code>vmafx-tune-go %s</code> &mdash; %d input(s)</p>\n",
		html.EscapeString(ToolVersion), len(reports))

	for _, mr := range reports {
		switch mr.Kind {
		case KindCompare:
			renderCompareHTML(&body, mr)
		case KindLadder:
			renderLadderHTML(&body, mr)
		}
	}

	return fmt.Sprintf(htmlPageTemplate,
		"vmafx-tune report",
		htmlCSS,
		body.String(),
	)
}

func renderCompareHTML(sb *strings.Builder, mr MultiReport) {
	fmt.Fprintf(sb, "<h2>Compare &mdash; <code>%s</code></h2>\n",
		html.EscapeString(mr.Src))
	fmt.Fprintf(sb, "<p class=\"meta\">Input: <code>%s</code> &bull; Tool: <code>%s</code>"+
		" &bull; Target VMAF: %g &bull; Wall time: %.1f ms</p>\n",
		html.EscapeString(mr.InputPath), html.EscapeString(mr.ToolVersion),
		mr.TargetVMAF, mr.WallTimeMS)

	sorted := make([]WireRow, len(mr.CompareRows))
	copy(sorted, mr.CompareRows)
	sort.SliceStable(sorted, func(i, j int) bool {
		oki, okj := sorted[i].OK, sorted[j].OK
		if oki != okj {
			return oki
		}
		if oki {
			bi := float64PtrOr(sorted[i].BitratekBps, math.MaxFloat64)
			bj := float64PtrOr(sorted[j].BitratekBps, math.MaxFloat64)
			return bi < bj
		}
		return false
	})

	sb.WriteString("<table>\n<thead><tr>")
	for _, h := range []string{"Rank", "Codec", "Encoder", "Best CRF",
		"Bitrate (kbps)", "Encode time (ms)", "VMAF", "Status"} {
		fmt.Fprintf(sb, "<th>%s</th>", h)
	}
	sb.WriteString("</tr></thead>\n<tbody>\n")

	rank := 0
	for _, row := range sorted {
		var rankCell, statusClass, statusText string
		if row.OK {
			rank++
			rankCell = fmt.Sprintf("%d", rank)
			statusClass = "ok"
			statusText = "ok"
		} else {
			rankCell = "&mdash;"
			statusClass = "fail"
			statusText = "fail: " + html.EscapeString(row.Error)
		}
		crfStr := "&mdash;"
		if row.BestCRF >= 0 {
			crfStr = fmt.Sprintf("%d", row.BestCRF)
		}
		bitrateStr := fmtFloat(row.BitratekBps, "%.1f")
		encTimeStr := fmtFloat(row.EncodeTimeMS, "%.1f")
		vmafStr := fmtFloat(row.VMAFScore, "%.2f")
		encoder := html.EscapeString(row.EncoderVersion)
		if encoder == "" {
			encoder = "&mdash;"
		}
		fmt.Fprintf(sb, "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td>"+
			"<td>%s</td><td>%s</td><td>%s</td><td class=%q>%s</td></tr>\n",
			rankCell, html.EscapeString(row.Codec), encoder, crfStr,
			bitrateStr, encTimeStr, vmafStr, statusClass, statusText)
	}
	sb.WriteString("</tbody></table>\n")
}

func renderLadderHTML(sb *strings.Builder, mr MultiReport) {
	fmt.Fprintf(sb, "<h2>Ladder &mdash; <code>%s</code></h2>\n",
		html.EscapeString(mr.Src))
	fmt.Fprintf(sb, "<p class=\"meta\">Input: <code>%s</code> &bull; Tool: <code>%s</code>"+
		" &bull; Encoder: <code>%s</code> &bull; Wall time: %.1f ms</p>\n",
		html.EscapeString(mr.InputPath), html.EscapeString(mr.ToolVersion),
		html.EscapeString(mr.LadderEncoder), mr.WallTimeMS)

	if len(mr.LadderRenditions) == 0 {
		sb.WriteString("<p><em>No renditions selected.</em></p>\n")
		return
	}

	sb.WriteString("<table>\n<thead><tr>")
	for _, h := range []string{"Rank", "Resolution", "Bitrate (kbps)", "VMAF", "CRF"} {
		fmt.Fprintf(sb, "<th>%s</th>", h)
	}
	sb.WriteString("</tr></thead>\n<tbody>\n")
	for i, r := range mr.LadderRenditions {
		fmt.Fprintf(sb, "<tr><td>%d</td><td>%dx%d</td><td>%.1f</td><td>%.2f</td><td>%d</td></tr>\n",
			i+1, r.Width, r.Height, r.BitratekBps, r.VMAF, r.CRF)
	}
	sb.WriteString("</tbody></table>\n")
}

// ---- helpers ----

// float64PtrOr dereferences p or returns fallback when p is nil.
func float64PtrOr(p *float64, fallback float64) float64 {
	if p == nil {
		return fallback
	}
	return *p
}

// fmtFloat formats *float64 using fmt when non-nil and not NaN/Inf, else "—".
func fmtFloat(p *float64, fmtStr string) string {
	if p == nil || math.IsNaN(*p) || math.IsInf(*p, 0) || *p < 0 {
		return "—"
	}
	return fmt.Sprintf(fmtStr, *p)
}
