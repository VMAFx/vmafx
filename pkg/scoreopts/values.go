// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package scoreopts

import (
	"fmt"
	"math"
	"strconv"
	"strings"
)

// Values holds option values keyed by option name (not by surface spelling).
// A value is a bool, a string, an integer type, a float64 or a []string.
type Values map[string]any

// maxListLen bounds a repeated value (HISS-02); a request naming more
// features than this is refused.
const maxListLen = 64

// Check validates `value` for option `o` on `surface`: its type, its bounds
// or allowed values, and a reserved option's default.
//
// An error reads "invalid <name> <value>: <reason>", where name is the
// surface's spelling (the MCP argument name on "mcp", the option name
// elsewhere).
func (o *Option) Check(surface string, value any) error {
	if err := o.check(surface, value); err != nil {
		return fmt.Errorf("invalid %s %v: %w", o.Label(surface), value, err)
	}
	return nil
}

// Label is the option's spelling on a surface.
func (o *Option) Label(surface string) string {
	if surface == "mcp" && o.MCP != "" {
		return o.MCP
	}
	return o.Name
}

func (o *Option) check(surface string, value any) error {
	if o.Repeats(surface) || o.Type == "flags" {
		return o.checkList(value)
	}
	if err := checkScalar(o.Schema, value); err != nil {
		return err
	}
	if o.Reserved != "" && !sameValue(value, o.DefaultOn(surface)) {
		return fmt.Errorf("%s; only %v is accepted", o.Reserved, o.DefaultOn(surface))
	}
	return nil
}

func (o *Option) checkList(value any) error {
	items, ok := value.([]string)
	if !ok {
		return fmt.Errorf("expected a list of strings")
	}
	if len(items) > maxListLen {
		return fmt.Errorf("at most %d values", maxListLen)
	}
	item := o.Schema
	if o.Schema.Items != nil {
		item = *o.Schema.Items
	}
	for _, v := range items {
		if v == "" {
			return fmt.Errorf("empty value")
		}
		if err := checkScalar(item, v); err != nil {
			return err
		}
	}
	return nil
}

func checkScalar(schema Schema, value any) error {
	switch schema.Type {
	case "boolean":
		if _, ok := value.(bool); !ok {
			return fmt.Errorf("expected a boolean")
		}
		return nil
	case "string":
		text, ok := value.(string)
		if !ok {
			return fmt.Errorf("expected a string")
		}
		return checkEnum(schema, text)
	case "integer", "number":
		return checkNumber(schema, value)
	default:
		return fmt.Errorf("unsupported schema type %q", schema.Type)
	}
}

func checkEnum(schema Schema, value any) error {
	if len(schema.Enum) == 0 {
		return nil
	}
	for _, allowed := range schema.Enum {
		if sameValue(value, allowed) {
			return nil
		}
	}
	return fmt.Errorf("must be one of %s", enumText(schema.Enum))
}

func enumText(values []any) string {
	parts := make([]string, 0, len(values))
	for _, v := range values {
		parts = append(parts, Text(v))
	}
	return strings.Join(parts, "|")
}

func checkNumber(schema Schema, value any) error {
	number, ok := toFloat(value)
	if !ok {
		return fmt.Errorf("expected a number")
	}
	if schema.Type == "integer" && number != math.Trunc(number) {
		return fmt.Errorf("expected an integer")
	}
	if schema.Minimum != nil && number < *schema.Minimum {
		return fmt.Errorf("must be >= %s", Text(*schema.Minimum))
	}
	if schema.Maximum != nil && number > *schema.Maximum {
		return fmt.Errorf("must be <= %s", Text(*schema.Maximum))
	}
	return checkEnum(schema, number)
}

// toFloat widens every number type a value may arrive as.
func toFloat(value any) (float64, bool) {
	switch v := value.(type) {
	case float64:
		return v, true
	case float32:
		return float64(v), true
	case int:
		return float64(v), true
	case int32:
		return float64(v), true
	case int64:
		return float64(v), true
	case uint32:
		return float64(v), true
	case uint64:
		return float64(v), true
	default:
		return 0, false
	}
}

// sameValue compares a value with a default or an enum entry decoded from JSON
// (numbers there are float64).
func sameValue(a, b any) bool {
	if fa, ok := toFloat(a); ok {
		fb, okB := toFloat(b)
		return okB && fa == fb
	}
	return fmt.Sprint(a) == fmt.Sprint(b)
}

// Text is the command-line form of a scalar value: integers in decimal,
// floats in the shortest form that reads back the same, strings as they are.
func Text(value any) string {
	switch v := value.(type) {
	case string:
		return v
	case bool:
		return strconv.FormatBool(v)
	case float64:
		return floatText(v)
	case float32:
		return floatText(float64(v))
	}
	if number, ok := toFloat(value); ok {
		return strconv.FormatFloat(number, 'f', -1, 64)
	}
	return fmt.Sprint(value)
}

// floatText writes a whole float as an integer and any other float in the
// shortest form that reads back the same.
func floatText(v float64) string {
	if v == math.Trunc(v) && math.Abs(v) < 1e15 {
		return strconv.FormatInt(int64(v), 10)
	}
	return strconv.FormatFloat(v, 'g', -1, 64)
}

// ExtraArgs is the `vmaf` flags of every set option whose argument-vector
// stage is "extra", in definition order (the order both MCP servers use).
func (d *Document) ExtraArgs(values Values) []string {
	var argv []string
	for i := range d.Argv {
		entry := &d.Argv[i]
		value, set := values[entry.Option]
		if entry.Stage != "extra" || !set {
			continue
		}
		argv = append(argv, entry.Args(value)...)
	}
	return argv
}

// Args is the flags one value of the entry's option becomes.
func (e *ArgvEntry) Args(value any) []string {
	switch e.Form {
	case "switch":
		if on, ok := value.(bool); ok && on {
			return []string{e.Flag}
		}
		return nil
	case "repeat":
		items, _ := value.([]string)
		out := make([]string, 0, 2*len(items))
		for _, item := range items {
			out = append(out, e.Flag, item)
		}
		return out
	case "choice":
		if flag, ok := e.Values[Text(value)]; ok {
			return []string{flag}
		}
		return nil
	case "value":
		return []string{e.Flag, Text(value)}
	default:
		return nil
	}
}

// ModelSpec appends the model-spec suffixes of the set options (the stage
// "core" entries of form "suffix") to `model`, in definition order, skipping a
// suffix the model already carries.
func (d *Document) ModelSpec(model string, values Values) string {
	for i := range d.Argv {
		entry := &d.Argv[i]
		value, set := values[entry.Option]
		if entry.Form != "suffix" || !set {
			continue
		}
		suffix := entry.Suffix
		if on, isBool := value.(bool); isBool {
			if !on {
				continue
			}
		} else {
			suffix = strings.ReplaceAll(suffix, "{}", Text(value))
		}
		if !strings.Contains(model, suffix) {
			model += suffix
		}
	}
	return model
}
