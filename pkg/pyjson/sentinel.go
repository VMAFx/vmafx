// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/pyjson/sentinel.go — reading CPython-written JSON back with
// encoding/json.
//
// A line written by json.dumps can carry the bare NaN / Infinity / -Infinity
// tokens, which encoding/json rejects. The reader substitutes quoted sentinel
// strings for the tokens before parsing and swaps the sentinels back for
// float64 values afterwards.

package pyjson

import (
	"math"
	"strings"
)

// Sentinel strings substituted for the bare non-finite tokens before handing
// a CPython-written JSON line to encoding/json. Each starts with a NUL, which
// json.dumps would have escaped as the six-character \u0000 sequence in any
// genuine string, so the substitution can never collide with real data.
const (
	sentinelNaN    = "\x00vmafx-nan"
	sentinelPosInf = "\x00vmafx-inf"
	sentinelNegInf = "\x00vmafx-neg-inf"
)

// SanitizeNonFinite rewrites the bare NaN / Infinity / -Infinity tokens a
// CPython-written JSON line can carry into quoted sentinel strings, so
// encoding/json can parse the line. Tokens inside string literals are left
// alone; ResolveSentinels converts the sentinels back to float64 after
// unmarshalling.
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
			b.WriteString(EncodeString(sentinelNaN))
			i += 2
		case strings.HasPrefix(line[i:], "-Infinity"):
			b.WriteString(EncodeString(sentinelNegInf))
			i += 8
		case strings.HasPrefix(line[i:], "Infinity"):
			b.WriteString(EncodeString(sentinelPosInf))
			i += 7
		default:
			b.WriteByte(c)
		}
	}
	return b.String()
}

// ResolveSentinels walks a decoded JSON value and swaps every non-finite
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
