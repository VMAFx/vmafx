// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
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
// Containers are rewritten in place, so the walk only has to remember which
// containers it still owes a visit. That is an explicit work stack rather than
// the call stack (HISS-01); a decoded JSON document is a tree, so every node
// is pushed at most once and the stack drains.
func ResolveSentinels(v any) any {
	root := resolveSentinel(v)
	stack := []any{root}
	for len(stack) > 0 {
		node := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		switch t := node.(type) {
		case []any:
			for i := range t {
				t[i] = resolveSentinel(t[i])
				stack = appendContainer(stack, t[i])
			}
		case map[string]any:
			for k := range t {
				t[k] = resolveSentinel(t[k])
				stack = appendContainer(stack, t[k])
			}
		}
	}
	return root
}

// resolveSentinel swaps one sentinel string for the float64 it stands in for.
// Every other value, container or scalar, passes through untouched.
func resolveSentinel(v any) any {
	s, ok := v.(string)
	if !ok {
		return v
	}
	switch s {
	case sentinelNaN:
		return math.NaN()
	case sentinelPosInf:
		return math.Inf(1)
	case sentinelNegInf:
		return math.Inf(-1)
	}
	return s
}

// appendContainer queues v for a later visit when it is a JSON container.
func appendContainer(stack []any, v any) []any {
	switch v.(type) {
	case []any, map[string]any:
		return append(stack, v)
	}
	return stack
}
