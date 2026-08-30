// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package pyjson renders Go values the way Python's json.dumps does.
//
// The vmafx-tune subcommands emit JSON that scripts, CI jobs and the fork's
// own Python tooling already parse. Keeping the Go port's payloads
// byte-identical to the Python originals means those consumers do not have to
// care which binary produced a file. Three differences make encoding/json
// unable to do that on its own:
//
//   - Whole-valued floats. Python renders 93.0 as "93.0"; encoding/json
//     renders it as "93". Both parse to the same number, but a byte diff
//     against a stored fixture fails, and a VMAF target that suddenly looks
//     like an integer reads wrong in a report.
//   - Non-finite floats. A feature aggregate the run did not measure is NaN
//     by design (ADR-0366). Python writes bare NaN / Infinity / -Infinity;
//     encoding/json refuses to marshal them at all.
//   - Key order. Python's json.dumps(..., sort_keys=True) sorts; a Go struct
//     marshals in field-declaration order. Handlers that pass sort_keys need
//     the sorted form.
//
// Everything else follows json.dumps: two-space indent with ", " / ": "
// separators when compact, ": " after keys when indented, and standard string
// escaping (delegated to encoding/json so the escape table cannot drift).
package pyjson

import (
	"encoding/json"
	"fmt"
	"math"
	"reflect"
	"sort"
	"strconv"
	"strings"
)

// Options controls the rendering.
type Options struct {
	// SortKeys sorts object keys, matching json.dumps(sort_keys=True).
	// Struct fields are otherwise emitted in declaration order, which is how
	// the Python handlers' dict literals are ordered.
	SortKeys bool
	// Indent is the per-level indent string. Empty renders compactly with
	// Python's ", " / ": " separators; "  " matches json.dumps(indent=2).
	Indent string
}

// Marshal renders v.
func Marshal(v any, opts Options) ([]byte, error) {
	var sb strings.Builder
	if err := encode(&sb, reflect.ValueOf(v), opts, 0); err != nil {
		return nil, err
	}
	return []byte(sb.String()), nil
}

// MarshalIndent renders v with json.dumps(indent=2) formatting.
func MarshalIndent(v any, sortKeys bool) ([]byte, error) {
	return Marshal(v, Options{SortKeys: sortKeys, Indent: "  "})
}

// member is one object entry awaiting emission.
type member struct {
	key   string
	value reflect.Value
}

// encode writes v at the given nesting depth.
func encode(sb *strings.Builder, v reflect.Value, opts Options, depth int) error {
	if !v.IsValid() {
		sb.WriteString("null")
		return nil
	}
	switch v.Kind() {
	case reflect.Interface, reflect.Pointer:
		if v.IsNil() {
			sb.WriteString("null")
			return nil
		}
		return encode(sb, v.Elem(), opts, depth)

	case reflect.Float32, reflect.Float64:
		sb.WriteString(FormatFloat(v.Float()))
		return nil

	case reflect.String:
		sb.WriteString(EncodeString(v.String()))
		return nil

	case reflect.Struct:
		return encodeObject(sb, structMembers(v, opts), opts, depth)

	case reflect.Map:
		if v.IsNil() {
			sb.WriteString("null")
			return nil
		}
		members, err := mapMembers(v)
		if err != nil {
			return err
		}
		// Go map iteration is unordered, so keys are always sorted here:
		// there is no insertion order to preserve, and a stable output is
		// worth more than mimicking an order that does not exist.
		sort.Slice(members, func(i, j int) bool { return members[i].key < members[j].key })
		return encodeObject(sb, members, opts, depth)

	case reflect.Slice:
		if v.IsNil() {
			sb.WriteString("null")
			return nil
		}
		return encodeArray(sb, v, opts, depth)

	case reflect.Array:
		return encodeArray(sb, v, opts, depth)

	default:
		blob, err := json.Marshal(v.Interface())
		if err != nil {
			return fmt.Errorf("pyjson: marshal %s: %w", v.Type(), err)
		}
		sb.Write(blob)
		return nil
	}
}

// structMembers collects a struct's JSON-visible fields in declaration order,
// honouring the json tag's name, "-" and omitempty.
func structMembers(v reflect.Value, opts Options) []member {
	t := v.Type()
	out := make([]member, 0, t.NumField())
	for i := 0; i < t.NumField(); i++ {
		field := t.Field(i)
		if !field.IsExported() {
			continue
		}
		tag := field.Tag.Get("json")
		if tag == "-" {
			continue
		}
		name, rest, _ := strings.Cut(tag, ",")
		if name == "" {
			name = field.Name
		}
		value := v.Field(i)
		if strings.Contains(rest, "omitempty") && isEmpty(value) {
			continue
		}
		out = append(out, member{key: name, value: value})
	}
	if opts.SortKeys {
		sort.Slice(out, func(i, j int) bool { return out[i].key < out[j].key })
	}
	return out
}

// mapMembers collects a map's entries. Only string-keyed maps are supported,
// which is all the payloads use.
func mapMembers(v reflect.Value) ([]member, error) {
	if v.Type().Key().Kind() != reflect.String {
		return nil, fmt.Errorf("pyjson: unsupported map key type %s", v.Type().Key())
	}
	out := make([]member, 0, v.Len())
	iter := v.MapRange()
	for iter.Next() {
		out = append(out, member{key: iter.Key().String(), value: iter.Value()})
	}
	return out, nil
}

// isEmpty implements encoding/json's omitempty predicate.
func isEmpty(v reflect.Value) bool {
	switch v.Kind() {
	case reflect.Array, reflect.Map, reflect.Slice, reflect.String:
		return v.Len() == 0
	case reflect.Bool:
		return !v.Bool()
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return v.Int() == 0
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		return v.Uint() == 0
	case reflect.Float32, reflect.Float64:
		return v.Float() == 0
	case reflect.Interface, reflect.Pointer:
		return v.IsNil()
	default:
		return false
	}
}

// encodeObject writes an object with the configured separators.
func encodeObject(sb *strings.Builder, members []member, opts Options, depth int) error {
	if len(members) == 0 {
		sb.WriteString("{}")
		return nil
	}
	sb.WriteByte('{')
	for i, m := range members {
		if i > 0 {
			sb.WriteByte(',')
			if opts.Indent == "" {
				sb.WriteByte(' ')
			}
		}
		writeNewlineIndent(sb, opts, depth+1)
		sb.WriteString(EncodeString(m.key))
		sb.WriteString(": ")
		if err := encode(sb, m.value, opts, depth+1); err != nil {
			return err
		}
	}
	writeNewlineIndent(sb, opts, depth)
	sb.WriteByte('}')
	return nil
}

// encodeArray writes an array with the configured separators.
func encodeArray(sb *strings.Builder, v reflect.Value, opts Options, depth int) error {
	if v.Len() == 0 {
		sb.WriteString("[]")
		return nil
	}
	sb.WriteByte('[')
	for i := 0; i < v.Len(); i++ {
		if i > 0 {
			sb.WriteByte(',')
			if opts.Indent == "" {
				sb.WriteByte(' ')
			}
		}
		writeNewlineIndent(sb, opts, depth+1)
		if err := encode(sb, v.Index(i), opts, depth+1); err != nil {
			return err
		}
	}
	writeNewlineIndent(sb, opts, depth)
	sb.WriteByte(']')
	return nil
}

// writeNewlineIndent emits the newline plus indent for one nesting level, or
// nothing in compact mode.
func writeNewlineIndent(sb *strings.Builder, opts Options, depth int) {
	if opts.Indent == "" {
		return
	}
	sb.WriteByte('\n')
	for i := 0; i < depth; i++ {
		sb.WriteString(opts.Indent)
	}
}

// EncodeString renders a Go string as a JSON string literal the way Python's
// json.dumps does with its default ensure_ascii=True.
//
// encoding/json is not usable here for two reasons, both of which produce
// bytes Python never would:
//
//   - It HTML-escapes <, > and & into <, > and & by default,
//     to be safe inside a <script> tag. Python does not, and a source path or
//     ffmpeg filter fragment containing & would diverge.
//   - It emits non-ASCII as raw UTF-8. Python escapes it to \uXXXX, with a
//     surrogate pair above the BMP.
func EncodeString(s string) string {
	var sb strings.Builder
	sb.Grow(len(s) + 2)
	sb.WriteByte('"')
	for _, r := range s {
		switch r {
		case '\\':
			sb.WriteString(`\\`)
		case '"':
			sb.WriteString(`\"`)
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
			case r > 0xFFFF:
				// Astral plane: Python emits a UTF-16 surrogate pair.
				r -= 0x10000
				hi := 0xD800 + (r >> 10)
				lo := 0xDC00 + (r & 0x3FF)
				fmt.Fprintf(&sb, `\u%04x\u%04x`, hi, lo)
			default:
				fmt.Fprintf(&sb, `\u%04x`, r)
			}
		}
	}
	sb.WriteByte('"')
	return sb.String()
}

// Python's repr switches a float to exponential notation outside this
// magnitude band; inside it, the value is written positionally. Go's 'g'
// verb switches far earlier (1e+06 where Python still writes 1000000.0), so
// the band is applied explicitly.
const (
	pyExpUpperBound = 1e16
	pyExpLowerBound = 1e-4
)

// FormatFloat renders a float the way Python's json.dumps does: bare NaN /
// Infinity / -Infinity for the non-finite values, and repr's shortest
// round-trip form otherwise — positional inside [1e-4, 1e16) with a trailing
// ".0" on whole values, exponential outside it.
func FormatFloat(v float64) string {
	switch {
	case math.IsNaN(v):
		return "NaN"
	case math.IsInf(v, 1):
		return "Infinity"
	case math.IsInf(v, -1):
		return "-Infinity"
	}
	magnitude := math.Abs(v)
	if v != 0 && (magnitude >= pyExpUpperBound || magnitude < pyExpLowerBound) {
		// Go's 'e' verb already pads the exponent to two digits, as Python's
		// repr does ("1e+16", "1.5e-07").
		return strconv.FormatFloat(v, 'e', -1, 64)
	}
	s := strconv.FormatFloat(v, 'f', -1, 64)
	if !strings.Contains(s, ".") {
		s += ".0"
	}
	return s
}
