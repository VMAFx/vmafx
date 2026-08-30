// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package pyjson renders Go value trees as byte-identical CPython
// `json.dumps(..., indent=2, sort_keys=True)` output.
//
// Why this exists: vmafx-tune-go replaces Python subcommands whose JSON
// payloads are a user-discoverable surface (they are diffed in CI, parsed by
// downstream tooling, and embedded in reports). Go's encoding/json differs
// from CPython's json module in four ways that all produce visible byte
// deltas:
//
//  1. Struct fields serialise in declaration order; CPython sorts keys when
//     sort_keys=True. (Go DOES sort map keys, so map trees are already close.)
//  2. Go escapes '<', '>' and '&' unless SetEscapeHTML(false); CPython never
//     escapes them. CPython escapes every non-ASCII rune (ensure_ascii=True);
//     Go emits them raw.
//  3. Go renders float64(92) as "92"; CPython's repr renders it "92.0", and
//     the two disagree on when to switch to exponent notation (Go flips at
//     1e6 for shortest-form 'g', CPython at 1e17).
//  4. Go has no equivalent of allow_nan=False (json.dumps raises) vs the
//     default allow_nan=True (emits the bare NaN / Infinity tokens).
//
// The encoder accepts the value shapes produced by json.Unmarshal into `any`
// with Decoder.UseNumber() — nil, bool, string, json.Number, []any and
// map[string]any — plus the Go numeric types a caller is likely to build a
// payload from. Anything else is an error rather than a silent mis-render.
//
// ADR-0705 / ADR-0730 / ADR-0770: staged Go port of vmaf-tune.
package pyjson

import (
	"encoding/json"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
	"unicode/utf16"
	"unicode/utf8"
)

// NaNPolicy selects how non-finite floats are rendered.
type NaNPolicy int

const (
	// NaNAsNull mirrors vmaftune.jsonio.dumps_strict: non-finite floats are
	// replaced by JSON null before encoding, so the payload stays valid
	// RFC 8259 (CPython would raise under allow_nan=False).
	NaNAsNull NaNPolicy = iota

	// NaNAsToken mirrors a bare json.dumps(...) call with the default
	// allow_nan=True: NaN, Infinity and -Infinity are emitted as bare
	// (non-standard) tokens exactly as CPython writes them.
	NaNAsToken
)

// Marshal renders v the way CPython's
// json.dumps(v, indent=2, sort_keys=True) would, without a trailing newline.
//
// Callers that need vmaftune.jsonio.dumps_strict semantics pass NaNAsNull;
// callers mirroring a plain json.dumps(..., indent=2, sort_keys=True) call
// pass NaNAsToken.
func Marshal(v any, policy NaNPolicy) (string, error) {
	var sb strings.Builder
	if err := encode(&sb, v, 0, policy); err != nil {
		return "", err
	}
	return sb.String(), nil
}

// encode writes one value at the given indent depth.
func encode(sb *strings.Builder, v any, depth int, policy NaNPolicy) error {
	switch t := v.(type) {
	case nil:
		sb.WriteString("null")
		return nil
	case bool:
		if t {
			sb.WriteString("true")
		} else {
			sb.WriteString("false")
		}
		return nil
	case string:
		sb.WriteString(EncodeString(t))
		return nil
	case json.Number:
		return encodeNumber(sb, t, policy)
	case int:
		sb.WriteString(strconv.FormatInt(int64(t), 10))
		return nil
	case int64:
		sb.WriteString(strconv.FormatInt(t, 10))
		return nil
	case float64:
		sb.WriteString(Float(t, policy))
		return nil
	case []any:
		return encodeArray(sb, t, depth, policy)
	case map[string]any:
		return encodeObject(sb, t, depth, policy)
	default:
		return fmt.Errorf("pyjson: unsupported type %T", v)
	}
}

// encodeNumber renders a json.Number the way CPython would after parsing the
// same literal: an integral literal round-trips as an int, everything else
// goes through the float repr.
func encodeNumber(sb *strings.Builder, n json.Number, policy NaNPolicy) error {
	if i, err := n.Int64(); err == nil {
		sb.WriteString(strconv.FormatInt(i, 10))
		return nil
	}
	f, err := n.Float64()
	if err != nil {
		return fmt.Errorf("pyjson: uninterpretable number %q: %w", n.String(), err)
	}
	sb.WriteString(Float(f, policy))
	return nil
}

// encodeArray writes a JSON array with CPython's indent=2 layout. An empty
// list renders as "[]" on one line, matching CPython.
func encodeArray(sb *strings.Builder, items []any, depth int, policy NaNPolicy) error {
	if len(items) == 0 {
		sb.WriteString("[]")
		return nil
	}
	sb.WriteString("[\n")
	inner := strings.Repeat("  ", depth+1)
	for i, item := range items {
		sb.WriteString(inner)
		if err := encode(sb, item, depth+1, policy); err != nil {
			return err
		}
		if i < len(items)-1 {
			sb.WriteByte(',')
		}
		sb.WriteByte('\n')
	}
	sb.WriteString(strings.Repeat("  ", depth))
	sb.WriteByte(']')
	return nil
}

// encodeObject writes a JSON object with sorted keys and CPython's indent=2
// layout. An empty dict renders as "{}" on one line, matching CPython.
func encodeObject(sb *strings.Builder, obj map[string]any, depth int, policy NaNPolicy) error {
	if len(obj) == 0 {
		sb.WriteString("{}")
		return nil
	}
	keys := make([]string, 0, len(obj))
	for k := range obj {
		keys = append(keys, k)
	}
	// CPython's sort_keys sorts by codepoint; Go's byte-wise sort over valid
	// UTF-8 is the same ordering.
	sort.Strings(keys)

	sb.WriteString("{\n")
	inner := strings.Repeat("  ", depth+1)
	for i, k := range keys {
		sb.WriteString(inner)
		sb.WriteString(EncodeString(k))
		sb.WriteString(": ")
		if err := encode(sb, obj[k], depth+1, policy); err != nil {
			return err
		}
		if i < len(keys)-1 {
			sb.WriteByte(',')
		}
		sb.WriteByte('\n')
	}
	sb.WriteString(strings.Repeat("  ", depth))
	sb.WriteByte('}')
	return nil
}

// EncodeString renders s as a CPython ensure_ascii=True JSON string literal:
// every rune outside the printable ASCII range 0x20..0x7e is escaped, '<',
// '>' and '&' are NOT escaped, and non-BMP runes become surrogate pairs.
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
			case r >= 0x20 && r <= 0x7e:
				sb.WriteRune(r)
			case r == utf8.RuneError:
				// An invalid byte decoded as U+FFFD; CPython's decoder would
				// have produced the same replacement char, which then escapes
				// to � under ensure_ascii.
				sb.WriteString(`�`)
			case r > 0xFFFF:
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

// Float renders f as CPython's json encoder would under the given policy.
func Float(f float64, policy NaNPolicy) string {
	if math.IsNaN(f) || math.IsInf(f, 0) {
		if policy == NaNAsNull {
			return "null"
		}
		switch {
		case math.IsNaN(f):
			return "NaN"
		case math.IsInf(f, 1):
			return "Infinity"
		default:
			return "-Infinity"
		}
	}
	return Repr(f)
}

// Repr renders a finite float64 the way CPython's repr() does.
//
// CPython formats the shortest round-tripping decimal, then picks fixed
// notation when the decimal point position `decpt` satisfies
// -4 < decpt <= 16 and exponent notation otherwise, always appending ".0" to
// a fixed-notation result with no fractional digits. Go's
// strconv.FormatFloat(f, 'g', -1, 64) uses the same shortest digits but flips
// to exponent notation at 1e6 and never appends ".0", so the two disagree on
// values as ordinary as 92.0 (Go "92") and 1234567.0 (Go "1.234567e+06").
func Repr(f float64) string {
	if f == 0 {
		if math.Signbit(f) {
			return "-0.0"
		}
		return "0.0"
	}

	neg := math.Signbit(f)
	// 'e' with precision -1 gives the shortest round-tripping digit string
	// plus a decimal exponent, which is exactly CPython's starting point.
	sci := strconv.FormatFloat(math.Abs(f), 'e', -1, 64)
	mantissa, expStr, _ := strings.Cut(sci, "e")
	exp, err := strconv.Atoi(expStr)
	if err != nil {
		// Unreachable for finite input; fall back rather than panic.
		return strconv.FormatFloat(f, 'g', -1, 64)
	}
	digits := strings.Replace(mantissa, ".", "", 1)
	decpt := exp + 1

	var body string
	switch {
	case decpt <= -4 || decpt > 16:
		body = sciBody(digits, exp)
	case decpt <= 0:
		body = "0." + strings.Repeat("0", -decpt) + digits
	case decpt >= len(digits):
		body = digits + strings.Repeat("0", decpt-len(digits)) + ".0"
	default:
		body = digits[:decpt] + "." + digits[decpt:]
	}
	if neg {
		return "-" + body
	}
	return body
}

// sciBody formats the exponent-notation half of Repr: one leading digit, an
// optional fractional tail, and a signed exponent zero-padded to at least two
// digits (CPython's "1e+16" / "1e-05" shape).
func sciBody(digits string, exp int) string {
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
