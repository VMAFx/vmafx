// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Package pyjson is a transitional alias of pkg/pyjson.
//
// CPython-compatible JSON has one implementation, pkg/pyjson (ADR-1137); the
// parallel port's four encoders — including the one that lived here — were
// measured redundant and folded into it. This package remains only because
// pkg/tune/sidecar and cmd/vmafx-tune/cmd/sidecar.go still import it and
// those files are owned by an in-flight PR (#1187, the sidecar Python-parity
// fix); repointing them here would conflict with it. Once #1187 lands, the
// sidecar imports move to pkg/pyjson and this package is deleted. Do not add
// anything to it.
package pyjson

import (
	"fmt"
	"strings"

	"github.com/VMAFx/vmafx/pkg/pyjson"
)

// Marshal renders v as CPython's json.dumps(v, indent=indent, sort_keys=True)
// would; indent <= 0 is the compact form. It is pkg/pyjson.Marshal with
// Options{SortKeys: true, Indent: indent}.
func Marshal(v any, indent int) (string, error) {
	out, err := pyjson.Marshal(v, pyjson.Options{SortKeys: true, Indent: indentString(indent)})
	return string(out), err
}

// MustMarshal is Marshal with the error promoted to a panic. Reserved for
// literal payloads whose shape is statically known.
func MustMarshal(v any, indent int) string {
	out, err := Marshal(v, indent)
	if err != nil {
		panic(fmt.Sprintf("pyjson: %v", err))
	}
	return out
}

// MarshalStrict is pkg/pyjson.MarshalStrict: vmaftune.jsonio.dumps_strict,
// with non-finite floats rendered as null.
func MarshalStrict(v any, indent int) (string, error) {
	return pyjson.MarshalStrict(v, indent)
}

// FormatFloat is pkg/pyjson.FormatFloat: CPython's float repr().
func FormatFloat(f float64) string {
	return pyjson.FormatFloat(f)
}

// EncodeString is pkg/pyjson.EncodeString: a quoted, ensure_ascii=True JSON
// string.
func EncodeString(s string) string {
	return pyjson.EncodeString(s)
}

// indentString converts CPython's integer indent into the per-level string.
func indentString(indent int) string {
	if indent <= 0 {
		return ""
	}
	return strings.Repeat(" ", indent)
}
