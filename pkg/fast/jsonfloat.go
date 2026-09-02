// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package fast

import (
	"encoding/json"
	"fmt"
	"math"
	"strconv"
	"strings"
)

// The `vmaf-tune fast` payload is emitted by Python as
// json.dumps(result, indent=2, sort_keys=True), and CPython's json module
// renders every float through float.__repr__. Go's encoding/json does not
// agree with that on two counts:
//
//	value            Python repr           Go encoding/json
//	90.0             "90.0"                "90"
//	1000000.0        "1000000.0"           "1e+06"
//
// Both differences show up in a real payload — target_vmaf is almost always
// integral, and predicted_kbps reaches 1e6 on 4K sources — so a Go-encoded
// payload would not be byte-identical to the Python one it is meant to
// replace. pythonFloatRepr reproduces CPython's formatting so the two
// implementations emit the same bytes.
//
// CPython's rule (Objects/floatobject.c float_repr ->
// PyOS_double_to_string(v, 'r', 0, Py_DTSF_ADD_DOT_0)) is: take the shortest
// round-tripping decimal digit string and its decimal exponent decpt, then
// use fixed notation when -4 < decpt <= 16 and exponential otherwise; in
// fixed notation append ".0" when the result would otherwise look like an
// integer.

// pythonFloatRepr renders v the way CPython's repr(float) does.
//
// Non-finite values are the caller's problem: Python would emit the
// non-standard NaN / Infinity tokens, so pythonFloatRepr returns an empty
// string for them and jsonNumber coerces them to JSON null instead — the
// same choice compare.go makes for its sweep payload, and the reason
// json.MarshalIndent cannot abort mid-encode on a corrupt score.
func pythonFloatRepr(v float64) string {
	if math.IsNaN(v) || math.IsInf(v, 0) {
		return ""
	}

	// strconv's 'e' format with precision -1 yields the shortest
	// round-tripping digit string, which is exactly what CPython's
	// shortest-repr mode produces.
	sci := strconv.FormatFloat(v, 'e', -1, 64)

	neg := strings.HasPrefix(sci, "-")
	if neg {
		sci = sci[1:]
	}
	mantissa, expPart, found := strings.Cut(sci, "e")
	if !found {
		// Unreachable for the 'e' verb, but keep the fallback total.
		return sci
	}
	exp, err := strconv.Atoi(expPart)
	if err != nil {
		return sci
	}
	digits := strings.Replace(mantissa, ".", "", 1)
	// decpt is the position of the decimal point relative to the digit
	// string: value == 0.<digits> * 10^decpt.
	decpt := exp + 1

	var out string
	switch {
	case decpt > -4 && decpt <= 16:
		out = fixedNotation(digits, decpt)
	default:
		out = expNotation(digits, decpt)
	}
	if neg {
		out = "-" + out
	}
	return out
}

// fixedNotation renders digits with the decimal point at decpt, appending
// ".0" when the result would otherwise read as an integer.
func fixedNotation(digits string, decpt int) string {
	switch {
	case decpt <= 0:
		return "0." + strings.Repeat("0", -decpt) + digits
	case decpt >= len(digits):
		return digits + strings.Repeat("0", decpt-len(digits)) + ".0"
	default:
		return digits[:decpt] + "." + digits[decpt:]
	}
}

// expNotation renders digits in CPython's exponential form: one leading
// digit, an optional fractional part, and a signed two-or-more-digit
// exponent (1e+16, 1e-05).
func expNotation(digits string, decpt int) string {
	var sb strings.Builder
	sb.WriteByte(digits[0])
	if len(digits) > 1 {
		sb.WriteByte('.')
		sb.WriteString(digits[1:])
	}
	exp := decpt - 1
	sign := byte('+')
	if exp < 0 {
		sign = '-'
		exp = -exp
	}
	fmt.Fprintf(&sb, "e%c%02d", sign, exp)
	return sb.String()
}

// jsonNumber renders v as a JSON number in CPython's repr formatting, or as
// JSON null when v is not finite.
func jsonNumber(v float64) json.RawMessage {
	s := pythonFloatRepr(v)
	if s == "" {
		return json.RawMessage("null")
	}
	return json.RawMessage(s)
}

// jsonNumberPtr renders an optional float: nil (the Python None) becomes
// JSON null.
func jsonNumberPtr(v *float64) json.RawMessage {
	if v == nil {
		return json.RawMessage("null")
	}
	return jsonNumber(*v)
}

// recommendWire is the on-the-wire shape of RecommendResult. Field order is
// alphabetical by JSON tag so the emitted document matches Python's
// sort_keys=True ordering; the float fields are pre-rendered so they match
// Python's repr formatting too.
type recommendWire struct {
	Encoder        string          `json:"encoder"`
	NTrials        int             `json:"n_trials"`
	Notes          string          `json:"notes"`
	PredictedKbps  json.RawMessage `json:"predicted_kbps"`
	PredictedVMAF  json.RawMessage `json:"predicted_vmaf"`
	ProxyVerifyGap json.RawMessage `json:"proxy_verify_gap"`
	RecommendedCRF int             `json:"recommended_crf"`
	ScoreBackend   string          `json:"score_backend,omitempty"`
	Smoke          bool            `json:"smoke"`
	TargetVMAF     json.RawMessage `json:"target_vmaf"`
	VerifyVMAF     json.RawMessage `json:"verify_vmaf"`
}

// MarshalJSON emits the payload byte-compatibly with the Python
// `vmaf-tune fast` output.
func (r RecommendResult) MarshalJSON() ([]byte, error) {
	return json.Marshal(recommendWire{
		Encoder:        r.Encoder,
		NTrials:        r.NTrials,
		Notes:          r.Notes,
		PredictedKbps:  jsonNumber(r.PredictedKbps),
		PredictedVMAF:  jsonNumber(r.PredictedVMAF),
		ProxyVerifyGap: jsonNumberPtr(r.ProxyVerifyGap),
		RecommendedCRF: r.RecommendedCRF,
		ScoreBackend:   r.ScoreBackend,
		Smoke:          r.Smoke,
		TargetVMAF:     jsonNumber(r.TargetVMAF),
		VerifyVMAF:     jsonNumberPtr(r.VerifyVMAF),
	})
}

// UnmarshalJSON reads a payload back, so a Go consumer can round-trip a
// document either implementation wrote.
func (r *RecommendResult) UnmarshalJSON(data []byte) error {
	var doc struct {
		Encoder        string   `json:"encoder"`
		NTrials        int      `json:"n_trials"`
		Notes          string   `json:"notes"`
		PredictedKbps  float64  `json:"predicted_kbps"`
		PredictedVMAF  float64  `json:"predicted_vmaf"`
		ProxyVerifyGap *float64 `json:"proxy_verify_gap"`
		RecommendedCRF int      `json:"recommended_crf"`
		ScoreBackend   string   `json:"score_backend"`
		Smoke          bool     `json:"smoke"`
		TargetVMAF     float64  `json:"target_vmaf"`
		VerifyVMAF     *float64 `json:"verify_vmaf"`
	}
	if err := json.Unmarshal(data, &doc); err != nil {
		return fmt.Errorf("fast: parse recommendation payload: %w", err)
	}
	*r = RecommendResult{
		Encoder:        doc.Encoder,
		NTrials:        doc.NTrials,
		Notes:          doc.Notes,
		PredictedKbps:  doc.PredictedKbps,
		PredictedVMAF:  doc.PredictedVMAF,
		ProxyVerifyGap: doc.ProxyVerifyGap,
		RecommendedCRF: doc.RecommendedCRF,
		ScoreBackend:   doc.ScoreBackend,
		Smoke:          doc.Smoke,
		TargetVMAF:     doc.TargetVMAF,
		VerifyVMAF:     doc.VerifyVMAF,
	}
	return nil
}
