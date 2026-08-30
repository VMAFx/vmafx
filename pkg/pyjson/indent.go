// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/pyjson/indent.go — the indented json.dumps form.
//
// vmaf-tune's operator surfaces (sidecar status / record / batch-record
// --json) emit json.dumps(payload, indent=2, sort_keys=True). CPython drops
// the trailing space from the item separator once an indent is set, so the
// separators become "," and ": " rather than ", " and ": ".

package pyjson

import (
	"fmt"
	"sort"
	"strings"
)

// MarshalIndentSorted renders an object as json.dumps(obj, indent=N,
// sort_keys=True) does.
//
// Nested objects and arrays are indented recursively; empty containers render
// as "{}" / "[]" on one line, matching CPython.
func MarshalIndentSorted(obj map[string]any, indent int) (string, error) {
	return marshalIndentValue(obj, indent, 0)
}

// marshalIndentValue renders one value at the given nesting depth.
func marshalIndentValue(v any, indent, depth int) (string, error) {
	pad := strings.Repeat(" ", indent*(depth+1))
	closePad := strings.Repeat(" ", indent*depth)

	switch t := v.(type) {
	case map[string]any:
		if len(t) == 0 {
			return "{}", nil
		}
		keys := make([]string, 0, len(t))
		for k := range t {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		parts := make([]string, 0, len(keys))
		for _, k := range keys {
			val, err := marshalIndentValue(t[k], indent, depth+1)
			if err != nil {
				return "", fmt.Errorf("key %q: %w", k, err)
			}
			parts = append(parts, pad+jsonString(k)+": "+val)
		}
		return "{\n" + strings.Join(parts, ",\n") + "\n" + closePad + "}", nil
	case []string:
		if len(t) == 0 {
			return "[]", nil
		}
		parts := make([]string, len(t))
		for i, s := range t {
			parts[i] = pad + jsonString(s)
		}
		return "[\n" + strings.Join(parts, ",\n") + "\n" + closePad + "]", nil
	case []any:
		if len(t) == 0 {
			return "[]", nil
		}
		parts := make([]string, len(t))
		for i, e := range t {
			s, err := marshalIndentValue(e, indent, depth+1)
			if err != nil {
				return "", err
			}
			parts[i] = pad + s
		}
		return "[\n" + strings.Join(parts, ",\n") + "\n" + closePad + "]", nil
	default:
		return jsonValue(v)
	}
}
