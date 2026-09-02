// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Package pyjson renders Go values byte-identically to CPython's json module.
//
// Every JSON surface the vmafx-tune Go port shares with the Python original —
// the corpus JSONL, the auto plan, the sidecar state and status payloads, the
// predict / prefilter / recommend reports, the benchmark and encode-profile
// summaries — is parsed by tooling that does not care which binary wrote it.
// That only works if the Go writer reproduces json.dumps to the byte, and
// encoding/json cannot, for four reasons:
//
//  1. Non-finite floats. json.dumps defaults to allow_nan=True and writes the
//     bare tokens NaN / Infinity / -Infinity (invalid RFC 8259, but what every
//     corpus row carries in an unmeasured feature column, ADR-0366).
//     encoding/json refuses to marshal them at all.
//  2. Float spelling. CPython uses repr(): the shortest round-tripping digits,
//     a mandatory ".0" on integral values, and the fixed/exponential switch at
//     decpt <= -4 || decpt > 16. Go's %g drops the ".0" and switches
//     elsewhere, so 93.0 renders as "93" and 1e15 as "1e+15".
//  3. Escaping. json.dumps defaults to ensure_ascii=True (every rune past
//     U+007E becomes a \uXXXX escape, surrogate-paired above the BMP) and
//     never escapes <, > or &; encoding/json does the opposite on both counts.
//  4. Layout. CPython's indent=N uses "," (no trailing space) between items
//     and ": " after keys, renders empty containers as "{}" / "[]", and with
//     sort_keys=True sorts by code point.
//
// # Value model
//
// Marshal walks any Go value reflectively: nil, bool, string, every integer
// kind, float32 / float64, json.Number, pointers and interfaces (nil renders
// as null), string-keyed maps (always sorted — a Go map has no insertion
// order to preserve), slices and arrays, and structs (honouring the
// encoding/json tag vocabulary: rename, "-", omitempty; declaration order
// unless Options.SortKeys). A nil slice or map renders as the empty
// container, not null: an empty Python list or dict is [] / {}, and callers
// model "absent" with a pointer or a nil interface. json.Number is re-emitted
// the way CPython would after parsing the same literal — an integral literal
// as an int, anything else through the float repr — so a payload that
// round-trips through json.Decoder.UseNumber() stays stable. Channels,
// functions, complex numbers and non-string map keys are errors rather than
// silent mis-encodes.
//
// # Non-finite policy
//
// Two Python entry points disagree on NaN: json.dumps writes the bare tokens,
// while vmaftune.jsonio.dumps_strict rewrites non-finite floats to null first
// so the payload stays valid RFC 8259 (the sidecar state file and the
// --execute JSONL rows). Options.NonFinite selects between them; the default
// is the json.dumps behaviour.
//
// This package is the single implementation of that contract (ADR-1137). The
// four independent encoders the parallel Go port produced were measured
// redundant — 200,000 random float64 bit patterns and 10,000 rendered
// payloads with zero disagreements — before being folded into this one.
package pyjson

import (
	"encoding/json"
	"fmt"
	"reflect"
	"sort"
	"strconv"
	"strings"
)

// NaNPolicy selects how a non-finite float is rendered.
type NaNPolicy int

const (
	// NaNAsToken mirrors json.dumps with its default allow_nan=True: NaN,
	// Infinity and -Infinity are emitted as bare tokens.
	NaNAsToken NaNPolicy = iota
	// NaNAsNull mirrors vmaftune.jsonio.dumps_strict: non-finite floats
	// become null, so the payload stays valid RFC 8259.
	NaNAsNull
)

// Options controls the rendering. The zero value is json.dumps(v) with its
// defaults: compact ", " / ": " separators, declaration-ordered struct
// fields, and the bare non-finite tokens.
type Options struct {
	// SortKeys sorts struct fields by key, matching sort_keys=True. Map keys
	// are always sorted.
	SortKeys bool
	// Indent is the per-level indent string; "" is the compact form and
	// "  " matches indent=2.
	Indent string
	// NonFinite selects the NaN / Infinity rendering.
	NonFinite NaNPolicy
}

// Marshal renders v under opts.
func Marshal(v any, opts Options) ([]byte, error) {
	e := encoder{opts: opts}
	if err := e.value(reflect.ValueOf(v), 0); err != nil {
		return nil, err
	}
	return []byte(e.sb.String()), nil
}

// MarshalIndent renders v as json.dumps(v, indent=2, sort_keys=sortKeys).
func MarshalIndent(v any, sortKeys bool) ([]byte, error) {
	return Marshal(v, Options{SortKeys: sortKeys, Indent: "  "})
}

// MarshalSorted renders obj as json.dumps(obj, sort_keys=True): compact, with
// ", " between members and ": " after keys.
func MarshalSorted(obj map[string]any) (string, error) {
	out, err := Marshal(obj, Options{SortKeys: true})
	return string(out), err
}

// MarshalIndentSorted renders obj as json.dumps(obj, indent=indent,
// sort_keys=True). indent <= 0 selects the compact form.
func MarshalIndentSorted(obj map[string]any, indent int) (string, error) {
	out, err := Marshal(obj, Options{SortKeys: true, Indent: indentString(indent)})
	return string(out), err
}

// MarshalStrict renders v as vmaftune.jsonio.dumps_strict does: sorted keys,
// the given indent (<= 0 for compact), and non-finite floats as null.
func MarshalStrict(v any, indent int) (string, error) {
	out, err := Marshal(v, Options{
		SortKeys: true, Indent: indentString(indent), NonFinite: NaNAsNull,
	})
	return string(out), err
}

// indentString converts CPython's integer indent into the per-level string.
func indentString(indent int) string {
	if indent <= 0 {
		return ""
	}
	return strings.Repeat(" ", indent)
}

// jsonNumberType is matched before the kind switch: json.Number is a string
// kind that must render as a number.
var jsonNumberType = reflect.TypeOf(json.Number(""))

// encoder accumulates one rendering.
type encoder struct {
	sb   strings.Builder
	opts Options
}

// member is one object entry awaiting emission.
type member struct {
	key   string
	value reflect.Value
}

// value writes v at the given nesting depth.
func (e *encoder) value(v reflect.Value, depth int) error {
	if !v.IsValid() {
		e.sb.WriteString("null")
		return nil
	}
	if v.Type() == jsonNumberType {
		return e.number(json.Number(v.String()))
	}
	switch v.Kind() {
	case reflect.Interface, reflect.Pointer:
		if v.IsNil() {
			e.sb.WriteString("null")
			return nil
		}
		return e.value(v.Elem(), depth)
	case reflect.Bool:
		e.sb.WriteString(strconv.FormatBool(v.Bool()))
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		e.sb.WriteString(strconv.FormatInt(v.Int(), 10))
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		e.sb.WriteString(strconv.FormatUint(v.Uint(), 10))
	case reflect.Float32, reflect.Float64:
		e.sb.WriteString(floatToken(v.Float(), e.opts.NonFinite))
	case reflect.String:
		e.sb.WriteString(EncodeString(v.String()))
	case reflect.Struct:
		return e.object(structMembers(v, e.opts.SortKeys), depth)
	case reflect.Map:
		members, err := mapMembers(v)
		if err != nil {
			return err
		}
		return e.object(members, depth)
	case reflect.Slice, reflect.Array:
		return e.array(v, depth)
	default:
		return fmt.Errorf("pyjson: unsupported type %s", v.Type())
	}
	return nil
}

// number re-emits a json.Number the way CPython would after parsing the same
// literal: an integral literal round-trips as an int, anything else goes
// through the float repr.
func (e *encoder) number(n json.Number) error {
	if i, err := n.Int64(); err == nil {
		e.sb.WriteString(strconv.FormatInt(i, 10))
		return nil
	}
	f, err := n.Float64()
	if err != nil {
		return fmt.Errorf("pyjson: uninterpretable number %q: %w", n.String(), err)
	}
	e.sb.WriteString(floatToken(f, e.opts.NonFinite))
	return nil
}

// structMembers collects a struct's JSON-visible fields in declaration order,
// honouring the json tag's name, "-" and omitempty. Embedded structs are not
// flattened; none of the ported payloads embed.
func structMembers(v reflect.Value, sortKeys bool) []member {
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
	if sortKeys {
		sort.Slice(out, func(i, j int) bool { return out[i].key < out[j].key })
	}
	return out
}

// mapMembers collects a map's entries, sorted by key. Only string-keyed maps
// are supported, which is all the payloads use. CPython's sort_keys sorts by
// code point, which is Go's byte-wise string comparison over valid UTF-8.
func mapMembers(v reflect.Value) ([]member, error) {
	if v.Type().Key().Kind() != reflect.String {
		return nil, fmt.Errorf("pyjson: unsupported map key type %s", v.Type().Key())
	}
	out := make([]member, 0, v.Len())
	iter := v.MapRange()
	for iter.Next() {
		out = append(out, member{key: iter.Key().String(), value: iter.Value()})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].key < out[j].key })
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
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		return v.Uint() == 0
	case reflect.Float32, reflect.Float64:
		return v.Float() == 0
	case reflect.Interface, reflect.Pointer:
		return v.IsNil()
	default:
		return false
	}
}

// object writes members with CPython's separators for the configured layout.
func (e *encoder) object(members []member, depth int) error {
	if len(members) == 0 {
		e.sb.WriteString("{}")
		return nil
	}
	e.sb.WriteByte('{')
	for i, m := range members {
		e.separator(i)
		e.newlineIndent(depth + 1)
		e.sb.WriteString(EncodeString(m.key))
		e.sb.WriteString(": ")
		if err := e.value(m.value, depth+1); err != nil {
			return fmt.Errorf("key %q: %w", m.key, err)
		}
	}
	e.newlineIndent(depth)
	e.sb.WriteByte('}')
	return nil
}

// array writes a slice or array with CPython's separators.
func (e *encoder) array(v reflect.Value, depth int) error {
	if v.Len() == 0 {
		e.sb.WriteString("[]")
		return nil
	}
	e.sb.WriteByte('[')
	for i := 0; i < v.Len(); i++ {
		e.separator(i)
		e.newlineIndent(depth + 1)
		if err := e.value(v.Index(i), depth+1); err != nil {
			return err
		}
	}
	e.newlineIndent(depth)
	e.sb.WriteByte(']')
	return nil
}

// separator writes the item separator before item i: ", " in the compact
// form, "," (the newline and indent follow) in the indented form.
func (e *encoder) separator(i int) {
	if i == 0 {
		return
	}
	e.sb.WriteByte(',')
	if e.opts.Indent == "" {
		e.sb.WriteByte(' ')
	}
}

// newlineIndent emits the newline plus indent for one nesting level, or
// nothing in the compact form.
func (e *encoder) newlineIndent(depth int) {
	if e.opts.Indent == "" {
		return
	}
	e.sb.WriteByte('\n')
	for i := 0; i < depth; i++ {
		e.sb.WriteString(e.opts.Indent)
	}
}
