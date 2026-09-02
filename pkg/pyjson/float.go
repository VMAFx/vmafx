// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/pyjson/float.go — CPython's float spelling.
//
// Two public forms exist because Python has two: repr(float) is what an
// f-string or str() produces (used for ffmpeg argv values such as "-r 24.0"),
// and json.dumps renders the same digits but spells the non-finite values as
// the bare NaN / Infinity / -Infinity tokens.

package pyjson

import (
	"math"
	"strconv"
	"strings"
)

// FloatRepr renders v the way CPython's repr(float) does.
//
// CPython takes the shortest decimal digit string that round-trips, then
// chooses the notation from the decimal-point position decpt: exponential
// when decpt <= -4 || decpt > 16, fixed otherwise. A fixed-notation result
// without fractional digits gets a trailing ".0" so the token still reads as
// a float. Go's %g uses the same digits but different thresholds and never
// appends the ".0", so 93.0 would render as "93" and 1e15 as "1e+15".
//
// Non-finite values spell as repr() does: "nan", "inf", "-inf". See
// FormatFloat for the json.dumps tokens.
func FloatRepr(v float64) string {
	switch {
	case math.IsNaN(v):
		return "nan"
	case math.IsInf(v, 1):
		return "inf"
	case math.IsInf(v, -1):
		return "-inf"
	}

	sign := ""
	if math.Signbit(v) {
		sign = "-"
		v = math.Abs(v)
	}

	// Shortest round-trip digits in scientific form, e.g. "1.5e+20" / "0e+00".
	sci := strconv.FormatFloat(v, 'e', -1, 64)
	mantissa, expText, _ := strings.Cut(sci, "e")
	exp, err := strconv.Atoi(expText)
	if err != nil {
		// Unreachable for a finite float; degrade to Go's own shortest form
		// rather than panicking on a malformed token.
		return sign + strconv.FormatFloat(v, 'g', -1, 64)
	}
	digits := strings.Replace(mantissa, ".", "", 1)
	decpt := exp + 1

	if decpt <= -4 || decpt > 16 {
		return sign + expNotation(digits, exp)
	}
	return sign + fixedNotation(digits, decpt)
}

// expNotation renders digits with an explicit exponent in CPython's
// "1e+16" / "1.5e-07" shape: the sign is always present and the exponent has
// at least two digits.
func expNotation(digits string, exp int) string {
	var sb strings.Builder
	sb.WriteByte(digits[0])
	if len(digits) > 1 {
		sb.WriteByte('.')
		sb.WriteString(digits[1:])
	}
	sb.WriteByte('e')
	if exp < 0 {
		sb.WriteByte('-')
		exp = -exp
	} else {
		sb.WriteByte('+')
	}
	if exp < 10 {
		sb.WriteByte('0')
	}
	sb.WriteString(strconv.Itoa(exp))
	return sb.String()
}

// fixedNotation renders digits with the decimal point at decpt, appending
// the ".0" CPython requires when the value lands on an integer boundary.
func fixedNotation(digits string, decpt int) string {
	nd := len(digits)
	switch {
	case decpt <= 0:
		return "0." + strings.Repeat("0", -decpt) + digits
	case decpt >= nd:
		return digits + strings.Repeat("0", decpt-nd) + ".0"
	default:
		return digits[:decpt] + "." + digits[decpt:]
	}
}

// FormatFloat renders v as json.dumps does with its default allow_nan=True:
// the bare NaN / Infinity / -Infinity tokens for the non-finite values and
// repr() for everything else.
func FormatFloat(v float64) string {
	return floatToken(v, NaNAsToken)
}

// floatToken renders v under the given non-finite policy.
func floatToken(v float64, policy NaNPolicy) string {
	switch {
	case math.IsNaN(v):
		if policy == NaNAsNull {
			return "null"
		}
		return "NaN"
	case math.IsInf(v, 1):
		if policy == NaNAsNull {
			return "null"
		}
		return "Infinity"
	case math.IsInf(v, -1):
		if policy == NaNAsNull {
			return "null"
		}
		return "-Infinity"
	}
	return FloatRepr(v)
}
