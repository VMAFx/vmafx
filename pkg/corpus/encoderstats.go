// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/encoderstats.go — x264 / x265 FFmpeg pass-1 stats parser.
//
// Go port of vmaftune.encoder_stats (ADR-0332). Captures the encoder-internal
// per-frame signal x264 / x265 already emit during a "-pass 1 -passlogfile"
// encode: predicted bitrate, QP, motion-vector cost, texture cost and
// macroblock-type counts. Those are exactly the signals the encoder's own
// rate-distortion engine saw, so feeding them back into the predictor closes
// the loop on encoder state rather than input pixels alone.
//
// The stats file is line-oriented UTF-8:
//
//	#options: <space-separated key=value config record>
//	in:0 out:0 type:I dur:2 q:25.23 aq:20.39 tex:14051 mv:1126 misc:5871 imb:80 pmb:0 smb:0 d:- ref:;
//
// x265 spells the same fields q-aq / icu / pcu / scu and emits fractional CTU
// counts; the tokenizer resolves both via field aliases.
//
// libvpx's first-pass stats use a binary layout (vpx_codec_pkt_t /
// VPX_CODEC_STATS_PKT) and stay opt-out until a binary packet parser lands.

package corpus

import (
	"bufio"
	"math"
	"os"
	"strconv"
	"strings"
)

// PerFrameStats is one stats-file frame record.
//
// Numeric fields are coerced to float (q, aq, coding-unit counts) or int (bit
// costs and frame indexes); unknown / missing tokens default to zero so the
// aggregator never fails on a partial row.
type PerFrameStats struct {
	InIdx     int
	OutIdx    int
	FrameType string // one of I, i, P, B, b
	QP        float64
	AQ        float64
	Tex       int
	MV        int
	Misc      int
	IMB       float64
	PMB       float64
	SMB       float64
}

// Bits is the total per-frame bit cost — tex + mv + misc.
func (s PerFrameStats) Bits() int { return s.Tex + s.MV + s.Misc }

// MBTotal is the total macroblock / CTU count for the frame.
func (s PerFrameStats) MBTotal() float64 { return s.IMB + s.PMB + s.SMB }

// IsIntra reports whether the frame is I or i (IDR / non-IDR intra).
func (s PerFrameStats) IsIntra() bool { return s.FrameType == "I" || s.FrameType == "i" }

// coerceInt is a tolerant int parse — empty / non-numeric tokens become zero.
// The stats file is encoder-emitted and well-formed in practice, but a
// malformed line at run-end (the encoder dying mid-write) must not poison the
// rest of the corpus.
func coerceInt(token string) int {
	v, err := strconv.Atoi(token)
	if err != nil {
		return 0
	}
	return v
}

// coerceFloat is the float counterpart of coerceInt.
func coerceFloat(token string) float64 {
	v, err := strconv.ParseFloat(token, 64)
	if err != nil {
		return 0.0
	}
	return v
}

// tokenizeStatsLine returns case-insensitive key/value tokens for a stats row.
func tokenizeStatsLine(text string) map[string]string {
	tokens := map[string]string{}
	for _, raw := range strings.Fields(strings.ReplaceAll(text, ",", " ")) {
		tok := strings.TrimRight(strings.TrimSpace(raw), ";")
		if tok == "" {
			continue
		}
		sep := ""
		switch {
		case strings.Contains(tok, ":"):
			sep = ":"
		case strings.Contains(tok, "="):
			sep = "="
		default:
			continue
		}
		key, value, _ := strings.Cut(tok, sep)
		if key == "" {
			continue
		}
		tokens[key] = value
		tokens[strings.ToLower(key)] = value
	}
	return tokens
}

// statsToken resolves the first present alias, defaulting to def.
func statsToken(tokens map[string]string, def string, names ...string) string {
	for _, name := range names {
		if v, ok := tokens[name]; ok {
			return v
		}
		if v, ok := tokens[strings.ToLower(name)]; ok {
			return v
		}
	}
	return def
}

// ParseStatsLine parses one stats-file line.
//
// It returns ok == false for the "#options:" header, blank lines, and any line
// without the mandatory in / type tokens.
func ParseStatsLine(line string) (PerFrameStats, bool) {
	text := strings.TrimSpace(line)
	if text == "" || strings.HasPrefix(text, "#") {
		return PerFrameStats{}, false
	}
	tokens := tokenizeStatsLine(text)
	if _, ok := tokens["in"]; !ok {
		return PerFrameStats{}, false
	}
	if _, ok := tokens["type"]; !ok {
		return PerFrameStats{}, false
	}
	return PerFrameStats{
		InIdx:     coerceInt(statsToken(tokens, "0", "in")),
		OutIdx:    coerceInt(statsToken(tokens, "0", "out")),
		FrameType: statsToken(tokens, "P", "type"),
		QP:        coerceFloat(statsToken(tokens, "0", "q")),
		AQ:        coerceFloat(statsToken(tokens, "0", "aq", "q-aq")),
		Tex:       coerceInt(statsToken(tokens, "0", "tex")),
		MV:        coerceInt(statsToken(tokens, "0", "mv")),
		Misc:      coerceInt(statsToken(tokens, "0", "misc")),
		IMB:       coerceFloat(statsToken(tokens, "0", "imb", "icu")),
		PMB:       coerceFloat(statsToken(tokens, "0", "pmb", "pcu")),
		SMB:       coerceFloat(statsToken(tokens, "0", "smb", "scu")),
	}, true
}

// ParseStatsFile loads every per-frame record from an x264 / x265 stats file.
// A missing or empty file yields an empty slice; the corpus row then records
// zero-valued aggregates.
func ParseStatsFile(path string) []PerFrameStats {
	f, err := os.Open(path) // #nosec G304 -- path is the driver-generated passlogfile sidecar.
	if err != nil {
		return nil
	}
	defer func() { _ = f.Close() }()

	var frames []PerFrameStats
	sc := bufio.NewScanner(f)
	// Stats lines are short, but a pathological encoder run could emit a
	// long ref: list; raise the token cap above bufio's 64 KiB default.
	sc.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	for sc.Scan() {
		if rec, ok := ParseStatsLine(sc.Text()); ok {
			frames = append(frames, rec)
		}
	}
	return frames
}

// mean returns the arithmetic mean, or 0.0 for an empty slice.
//
// The accumulation goes through pySum so the result is bit-identical to
// CPython's sum(values) / len(values) — see pysum.go for why a plain Go loop
// is not.
func mean(values []float64) float64 {
	if len(values) == 0 {
		return 0.0
	}
	return pySum(values) / float64(len(values))
}

// stddev returns the population standard deviation, or 0.0 for an empty /
// singleton slice.
//
// This mirrors encoder_stats._std exactly: the squared deviations are summed
// through the same compensated algorithm CPython's sum() applies to a
// generator of floats, then divided by n and square-rooted.
func stddev(values []float64) float64 {
	n := len(values)
	if n < 2 {
		return 0.0
	}
	mu := mean(values)
	squared := make([]float64, n)
	for i, v := range values {
		squared[i] = (v - mu) * (v - mu)
	}
	return math.Sqrt(pySum(squared) / float64(n))
}

// AggregateStats returns the ten enc_internal_* scalar columns for one encode.
// Empty input yields a fully-zero map so corpus rows stay schema-uniform.
func AggregateStats(frames []PerFrameStats) map[string]float64 {
	out := make(map[string]float64, len(EncoderStatsColumns))
	for _, col := range EncoderStatsColumns {
		out[col] = 0.0
	}
	if len(frames) == 0 {
		return out
	}

	qps := make([]float64, 0, len(frames))
	bits := make([]float64, 0, len(frames))
	mvs := make([]float64, 0, len(frames))
	var itex, ptex []float64
	var totalMB, totalIMB, totalSMB float64

	for _, f := range frames {
		qps = append(qps, f.QP)
		bits = append(bits, float64(f.Bits()))
		mvs = append(mvs, float64(f.MV))
		if f.IsIntra() {
			itex = append(itex, float64(f.Tex))
		} else {
			ptex = append(ptex, float64(f.Tex))
		}
		totalMB += f.MBTotal()
		totalIMB += f.IMB
		totalSMB += f.SMB
	}

	out["enc_internal_qp_mean"] = mean(qps)
	out["enc_internal_qp_std"] = stddev(qps)
	out["enc_internal_bits_mean"] = mean(bits)
	out["enc_internal_bits_std"] = stddev(bits)
	out["enc_internal_mv_mean"] = mean(mvs)
	out["enc_internal_mv_std"] = stddev(mvs)
	out["enc_internal_itex_mean"] = mean(itex)
	out["enc_internal_ptex_mean"] = mean(ptex)
	if totalMB > 0 {
		out["enc_internal_intra_ratio"] = totalIMB / totalMB
		out["enc_internal_skip_ratio"] = totalSMB / totalMB
	}
	return out
}
