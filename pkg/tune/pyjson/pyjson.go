// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Package pyjson renders JSON byte-identically to CPython's
// json.dumps(obj, indent=N, sort_keys=True).
//
// The Go port of vmaf-tune must emit the *same bytes* as the Python original
// for every user-discoverable JSON surface (ADR-0705 schema-forward
// invariant): the MCP server, the CI corpus collector, and the post-hoc
// speedup analysis all parse `vmaf-tune auto`'s plan JSON and the
// `vmaf-tune sidecar` status payloads. encoding/json cannot produce those
// bytes because it differs from CPython in four load-bearing ways:
//
//  1. Non-finite floats. CPython's default allow_nan=True emits the bare
//     tokens NaN / Infinity / -Infinity. vmaftune.auto.emit_plan_json uses
//     that default, and a non-smoke `auto` run always carries at least one
//     NaN (the uncalibrated conformal interval_width), so a Go emitter that
//     rejects NaN cannot round-trip the real output at all.
//  2. Float spelling. CPython uses repr(): shortest round-trip digits, a
//     mandatory ".0" on integral values, and the fixed/exponential switch at
//     decpt <= -4 || decpt > 16. Go's %g switches at a different threshold
//     (1e15 prints as "1e+15" in Go and "1000000000000000.0" in Python) and
//     never appends ".0".
//  3. Indent layout. CPython's indent=N uses "," (no trailing space) as the
//     item separator and ": " as the key separator, and renders empty
//     containers as "{}" / "[]" with no inner newline.
//  4. ensure_ascii=True. Every rune above U+007E is escaped as \uXXXX, with
//     surrogate pairs for astral planes.
//
// Values are the usual any-tree: map[string]any, []any, string, bool, nil,
// float64, and the integer kinds. Nothing else is accepted — Marshal returns
// an error rather than guessing, so a schema drift fails loudly in tests
// instead of silently emitting the wrong bytes.
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

// Marshal renders v the way CPython's json.dumps(v, indent=indent,
// sort_keys=True) would. Pass indent <= 0 for the compact single-line form
// (CPython's indent=None, which uses ", " and ": " as separators).
func Marshal(v any, indent int) (string, error) {
	var sb strings.Builder
	if err := encode(&sb, v, indent, 0); err != nil {
		return "", err
	}
	return sb.String(), nil
}

// MustMarshal is Marshal with the error promoted to a panic. Reserved for
// literal payloads built in this repo whose shape is statically known; never
// call it on a tree assembled from external input.
func MustMarshal(v any, indent int) string {
	out, err := Marshal(v, indent)
	if err != nil {
		panic(fmt.Sprintf("pyjson: %v", err))
	}
	return out
}

func encode(sb *strings.Builder, v any, indent, depth int) error {
	switch value := v.(type) {
	case nil:
		sb.WriteString("null")
		return nil
	case bool:
		if value {
			sb.WriteString("true")
		} else {
			sb.WriteString("false")
		}
		return nil
	case string:
		sb.WriteString(EncodeString(value))
		return nil
	case float64:
		sb.WriteString(FormatFloat(value))
		return nil
	case float32:
		sb.WriteString(FormatFloat(float64(value)))
		return nil
	case int:
		sb.WriteString(strconv.Itoa(value))
		return nil
	case int32:
		sb.WriteString(strconv.FormatInt(int64(value), 10))
		return nil
	case int64:
		sb.WriteString(strconv.FormatInt(value, 10))
		return nil
	case uint64:
		sb.WriteString(strconv.FormatUint(value, 10))
		return nil
	case []any:
		return encodeList(sb, value, indent, depth)
	case []string:
		items := make([]any, len(value))
		for i, s := range value {
			items[i] = s
		}
		return encodeList(sb, items, indent, depth)
	case map[string]any:
		return encodeObject(sb, value, indent, depth)
	default:
		return fmt.Errorf("pyjson: unsupported type %T", v)
	}
}

func encodeList(sb *strings.Builder, items []any, indent, depth int) error {
	if len(items) == 0 {
		sb.WriteString("[]")
		return nil
	}
	inner, closing, sep := layout(indent, depth)
	sb.WriteByte('[')
	for i, item := range items {
		if i > 0 {
			sb.WriteString(sep)
		}
		sb.WriteString(inner)
		if err := encode(sb, item, indent, depth+1); err != nil {
			return err
		}
	}
	sb.WriteString(closing)
	sb.WriteByte(']')
	return nil
}

func encodeObject(sb *strings.Builder, obj map[string]any, indent, depth int) error {
	if len(obj) == 0 {
		sb.WriteString("{}")
		return nil
	}
	keys := make([]string, 0, len(obj))
	for k := range obj {
		keys = append(keys, k)
	}
	// sort_keys=True. CPython sorts str keys by code point, which is exactly
	// Go's byte-wise string comparison for valid UTF-8.
	sort.Strings(keys)

	inner, closing, sep := layout(indent, depth)
	sb.WriteByte('{')
	for i, k := range keys {
		if i > 0 {
			sb.WriteString(sep)
		}
		sb.WriteString(inner)
		sb.WriteString(EncodeString(k))
		sb.WriteString(": ")
		if err := encode(sb, obj[k], indent, depth+1); err != nil {
			return err
		}
	}
	sb.WriteString(closing)
	sb.WriteByte('}')
	return nil
}

// layout returns the per-item prefix, the pre-close suffix, and the item
// separator for a container opened at depth. With indent<=0 this is CPython's
// compact form (separators default to ", " when indent is None).
func layout(indent, depth int) (inner, closing, sep string) {
	if indent <= 0 {
		return "", "", ", "
	}
	inner = "\n" + strings.Repeat(" ", indent*(depth+1))
	closing = "\n" + strings.Repeat(" ", indent*depth)
	return inner, closing, ","
}

// FormatFloat renders f exactly as CPython's repr() would inside json.dumps
// with allow_nan=True.
//
// CPython uses "shortest round-trip digits" (the same digit string Go's
// strconv produces with precision -1) and then picks fixed vs exponential
// notation from the decimal point position: exponential iff
// decpt <= -4 || decpt > 16, where decpt is the position of the decimal point
// relative to the first significant digit. Integral values in fixed notation
// get a trailing ".0" so the token still reads as a float.
func FormatFloat(f float64) string {
	switch {
	case math.IsNaN(f):
		return "NaN"
	case math.IsInf(f, 1):
		return "Infinity"
	case math.IsInf(f, -1):
		return "-Infinity"
	}

	sign := ""
	if math.Signbit(f) {
		sign = "-"
		f = math.Abs(f)
	}

	// Shortest round-trip digits in scientific form, e.g. "1.5e+20" / "0e+00".
	sci := strconv.FormatFloat(f, 'e', -1, 64)
	epos := strings.IndexByte(sci, 'e')
	mantissa := sci[:epos]
	exp, err := strconv.Atoi(sci[epos+1:])
	if err != nil {
		// Unreachable for a finite float; fall back to Go's own shortest form.
		return sign + strconv.FormatFloat(f, 'g', -1, 64)
	}
	digits := strings.Replace(mantissa, ".", "", 1)
	decpt := exp + 1

	if decpt <= -4 || decpt > 16 {
		return sign + expNotation(digits, exp)
	}
	return sign + fixedNotation(digits, decpt)
}

// expNotation renders digits with an explicit exponent, matching CPython's
// "1e+16" / "1.5e-07" spelling (sign always present, at least two digits).
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

// fixedNotation renders digits with the decimal point at decpt, appending the
// ".0" CPython requires when the value lands on an integer boundary.
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

// EncodeString renders s as a CPython ensure_ascii=True JSON string literal:
// the six short escapes, \u00XX for the remaining C0 controls, and \uXXXX
// (surrogate-paired above the BMP) for everything past U+007E.
func EncodeString(s string) string {
	var sb strings.Builder
	sb.Grow(len(s) + 2)
	sb.WriteByte('"')
	for _, r := range s {
		switch r {
		case '"':
			sb.WriteString(`\"`)
		case '\\':
			sb.WriteString(`\\`)
		case '\b':
			sb.WriteString(`\b`)
		case '\f':
			sb.WriteString(`\f`)
		case '\n':
			sb.WriteString(`\n`)
		case '\r':
			sb.WriteString(`\r`)
		case '\t':
			sb.WriteString(`\t`)
		default:
			switch {
			case r < 0x20:
				fmt.Fprintf(&sb, `\u%04x`, r)
			case r <= 0x7e:
				sb.WriteByte(byte(r))
			case r == utf8.RuneError:
				// Go decodes an invalid byte as U+FFFD; CPython would have
				// raised. Emitting the replacement char keeps the output
				// well-formed rather than truncating the payload.
				sb.WriteString(`�`)
			case r > 0xffff:
				hi, lo := utf16.EncodeRune(r)
				fmt.Fprintf(&sb, `\u%04x\u%04x`, hi, lo)
			default:
				fmt.Fprintf(&sb, `\u%04x`, r)
			}
		}
	}
	sb.WriteByte('"')
	return sb.String()
}

// NaNToNull recursively replaces every non-finite float with nil, mirroring
// vmaftune.jsonio.nan_to_none. The sidecar state file and the executor's
// JSONL rows go through this (dumps_strict) rather than the allow_nan path,
// so they stay portable RFC-8259.
func NaNToNull(v any) any {
	switch value := v.(type) {
	case float64:
		if math.IsNaN(value) || math.IsInf(value, 0) {
			return nil
		}
		return value
	case map[string]any:
		out := make(map[string]any, len(value))
		for k, item := range value {
			out[k] = NaNToNull(item)
		}
		return out
	case []any:
		out := make([]any, len(value))
		for i, item := range value {
			out[i] = NaNToNull(item)
		}
		return out
	default:
		return v
	}
}

// MarshalStrict mirrors vmaftune.jsonio.dumps_strict: non-finite floats are
// rendered as null instead of the NaN / Infinity tokens.
func MarshalStrict(v any, indent int) (string, error) {
	return Marshal(NaNToNull(v), indent)
}
