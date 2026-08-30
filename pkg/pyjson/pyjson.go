// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// Package pyjson renders JSON exactly as CPython's json module does.
//
// vmaf-tune's on-disk artefacts (the corpus JSONL, the sidecar --json payloads)
// are written by Python today and read by the Phase B/C trainers and by
// operator tooling, so a Go writer has to reproduce json.dumps byte-for-byte.
// Three CPython behaviours that encoding/json does not share:
//
//  1. Non-finite floats serialise as the bare tokens NaN / Infinity /
//     -Infinity. That is invalid RFC-8259 JSON, but json.loads accepts it and
//     every corpus row carries NaN in at least the canonical-6 columns when
//     libvmaf does not expose a pooled feature (ADR-0366). encoding/json
//     refuses to marshal them at all.
//  2. Floats render via repr(), which is the shortest round-tripping decimal
//     with a mandatory ".0" on integral values and a decimal-point / exponent
//     switch at decpt <= -4 or decpt > 16. Go's 'g' verb switches at different
//     thresholds and drops the trailing ".0".
//  3. json.dumps defaults to ensure_ascii=True, so every non-ASCII rune is
//     escaped as \uXXXX (surrogate pairs above the BMP). Go escapes <, > and &
//     instead and passes non-ASCII through as UTF-8.
//
// The encoder is deliberately narrow: it handles the scalar / string / bool /
// nil / []string / []any / map[string]any shapes these payloads hold. Anything
// else is an error rather than a silent mis-encode.
package pyjson

import (
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
	"unicode/utf16"
	"unicode/utf8"
)

// PyFloatRepr renders v the way CPython's repr(float) does.
//
// CPython uses the shortest decimal string that round-trips, then chooses
// between fixed and exponential notation on the decimal-point position:
// exponential when decpt <= -4 or decpt > 16, fixed otherwise. Integral values
// in fixed notation get a trailing ".0" so the token stays a float literal.
func FloatRepr(v float64) string {
	switch {
	case math.IsNaN(v):
		return "nan"
	case math.IsInf(v, 1):
		return "inf"
	case math.IsInf(v, -1):
		return "-inf"
	}
	// Shortest round-trip form in scientific notation, e.g. "-1.234e+05".
	sci := strconv.FormatFloat(v, 'e', -1, 64)

	neg := false
	if strings.HasPrefix(sci, "-") {
		neg = true
		sci = sci[1:]
	}
	ePos := strings.IndexByte(sci, 'e')
	mantissa := sci[:ePos]
	exp, err := strconv.Atoi(sci[ePos+1:])
	if err != nil {
		// Unreachable for FormatFloat output; degrade to Go's own shortest
		// form rather than panicking on a malformed token.
		return strconv.FormatFloat(v, 'g', -1, 64)
	}
	digits := strings.Replace(mantissa, ".", "", 1)
	decpt := exp + 1

	var b strings.Builder
	if neg {
		b.WriteByte('-')
	}
	if decpt <= -4 || decpt > 16 {
		// Exponential: d[0](.d[1:])e{+,-}NN with at least two exponent digits.
		b.WriteByte(digits[0])
		if len(digits) > 1 {
			b.WriteByte('.')
			b.WriteString(digits[1:])
		}
		b.WriteByte('e')
		e := decpt - 1
		if e < 0 {
			b.WriteByte('-')
			e = -e
		} else {
			b.WriteByte('+')
		}
		es := strconv.Itoa(e)
		if len(es) < 2 {
			b.WriteByte('0')
		}
		b.WriteString(es)
		return b.String()
	}
	switch {
	case decpt <= 0:
		b.WriteString("0.")
		b.WriteString(strings.Repeat("0", -decpt))
		b.WriteString(digits)
	case decpt >= len(digits):
		b.WriteString(digits)
		b.WriteString(strings.Repeat("0", decpt-len(digits)))
		b.WriteString(".0")
	default:
		b.WriteString(digits[:decpt])
		b.WriteByte('.')
		b.WriteString(digits[decpt:])
	}
	return b.String()
}

// pyJSONFloat renders a float as CPython's json encoder would: the bare
// non-finite tokens, otherwise repr().
func jsonFloat(v float64) string {
	switch {
	case math.IsNaN(v):
		return "NaN"
	case math.IsInf(v, 1):
		return "Infinity"
	case math.IsInf(v, -1):
		return "-Infinity"
	}
	return FloatRepr(v)
}

// pyJSONString renders s the way json.dumps(ensure_ascii=True) does.
func jsonString(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for _, r := range s {
		switch r {
		case '"':
			b.WriteString(`\"`)
			continue
		case '\\':
			b.WriteString(`\\`)
			continue
		case '\n':
			b.WriteString(`\n`)
			continue
		case '\r':
			b.WriteString(`\r`)
			continue
		case '\t':
			b.WriteString(`\t`)
			continue
		case '\b':
			b.WriteString(`\b`)
			continue
		case '\f':
			b.WriteString(`\f`)
			continue
		}
		switch {
		case r < 0x20:
			fmt.Fprintf(&b, `\u%04x`, r)
		case r < 0x7f:
			b.WriteRune(r)
		case r == utf8.RuneError:
			// Invalid UTF-8 byte; CPython would have raised on decode.
			// Emit the replacement character escape so the line stays
			// parseable rather than propagating raw garbage.
			b.WriteString(`�`)
		case r <= 0xffff:
			fmt.Fprintf(&b, `\u%04x`, r)
		default:
			hi, lo := utf16.EncodeRune(r)
			fmt.Fprintf(&b, `\u%04x\u%04x`, hi, lo)
		}
	}
	b.WriteByte('"')
	return b.String()
}

// pyJSONValue renders one JSON value in CPython's compact-with-separators
// default form (", " between items, ": " between key and value).
func jsonValue(v any) (string, error) {
	switch t := v.(type) {
	case nil:
		return "null", nil
	case bool:
		if t {
			return "true", nil
		}
		return "false", nil
	case string:
		return jsonString(t), nil
	case int:
		return strconv.Itoa(t), nil
	case int64:
		return strconv.FormatInt(t, 10), nil
	case float64:
		return jsonFloat(t), nil
	case []string:
		parts := make([]string, len(t))
		for i, s := range t {
			parts[i] = jsonString(s)
		}
		return "[" + strings.Join(parts, ", ") + "]", nil
	case []any:
		parts := make([]string, len(t))
		for i, e := range t {
			s, err := jsonValue(e)
			if err != nil {
				return "", err
			}
			parts[i] = s
		}
		return "[" + strings.Join(parts, ", ") + "]", nil
	case map[string]any:
		return objectSorted(t)
	default:
		return "", fmt.Errorf("pyjson: unsupported value type %T", v)
	}
}

// pyJSONObjectSorted renders a map as json.dumps(obj, sort_keys=True) does.
func objectSorted(m map[string]any) (string, error) {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	parts := make([]string, 0, len(keys))
	for _, k := range keys {
		val, err := jsonValue(m[k])
		if err != nil {
			return "", fmt.Errorf("key %q: %w", k, err)
		}
		parts = append(parts, jsonString(k)+": "+val)
	}
	return "{" + strings.Join(parts, ", ") + "}", nil
}

// MarshalSorted renders an object exactly as json.dumps(obj, sort_keys=True)
// would: keys sorted, ", " between items, ": " between key and value.
func MarshalSorted(obj map[string]any) (string, error) {
	return objectSorted(obj)
}

// Sentinel strings substituted for the bare non-finite tokens before handing a
// CPython-written JSON line to encoding/json. Each starts with a NUL, which
// json.dumps would have escaped as a six-character backslash-u escape in
// any genuine corpus string, so the substitution can never collide with real
// data.
const (
	sentinelNaN    = "\x00vmafx-nan"
	sentinelPosInf = "\x00vmafx-inf"
	sentinelNegInf = "\x00vmafx-neg-inf"
)

// sanitizeNonFinite rewrites the bare NaN / Infinity / -Infinity tokens a
// CPython-written JSON line can carry into quoted sentinel strings, so
// encoding/json can parse the line. Tokens inside string literals are left
// alone; resolveSentinels converts them back to float64 after unmarshalling.
func SanitizeNonFinite(line string) string {
	var b strings.Builder
	b.Grow(len(line))
	inString := false
	escaped := false
	for i := 0; i < len(line); i++ {
		c := line[i]
		if inString {
			b.WriteByte(c)
			switch {
			case escaped:
				escaped = false
			case c == '\\':
				escaped = true
			case c == '"':
				inString = false
			}
			continue
		}
		if c == '"' {
			inString = true
			b.WriteByte(c)
			continue
		}
		switch {
		case strings.HasPrefix(line[i:], "NaN"):
			b.WriteString(jsonString(sentinelNaN))
			i += 2
		case strings.HasPrefix(line[i:], "-Infinity"):
			b.WriteString(jsonString(sentinelNegInf))
			i += 8
		case strings.HasPrefix(line[i:], "Infinity"):
			b.WriteString(jsonString(sentinelPosInf))
			i += 7
		default:
			b.WriteByte(c)
		}
	}
	return b.String()
}

// resolveSentinels walks a decoded JSON value and swaps every non-finite
// sentinel string back for the float64 it stands in for.
func ResolveSentinels(v any) any {
	switch t := v.(type) {
	case string:
		switch t {
		case sentinelNaN:
			return math.NaN()
		case sentinelPosInf:
			return math.Inf(1)
		case sentinelNegInf:
			return math.Inf(-1)
		}
		return t
	case []any:
		for i := range t {
			t[i] = ResolveSentinels(t[i])
		}
		return t
	case map[string]any:
		for k := range t {
			t[k] = ResolveSentinels(t[k])
		}
		return t
	default:
		return v
	}
}
