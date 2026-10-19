// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package scoreopts

import (
	"fmt"

	"google.golang.org/protobuf/reflect/protoreflect"
)

// FromMCP reads the scoring options of an MCP call: every argument the
// generated schema names, checked against its option in definition order (so
// the first complaint is stable), keyed by option name. Arguments the
// generated schemas do not name are left to the caller.
func (d *Document) FromMCP(args map[string]any) (Values, error) {
	values := Values{}
	for i := range d.Options {
		option := &d.Options[i]
		raw, given := args[option.MCP]
		if option.MCP == "" || !given || raw == nil {
			continue
		}
		value, err := fromJSON(option, raw)
		if err != nil {
			return nil, err
		}
		if err := option.Check("mcp", value); err != nil {
			return nil, err
		}
		values[option.Name] = value
	}
	return values, nil
}

// fromJSON turns a decoded JSON value into the Go form Check expects: a list
// argument becomes a []string.
func fromJSON(option *Option, raw any) (any, error) {
	if !option.Repeats("mcp") && option.Type != "flags" {
		return raw, nil
	}
	items, ok := raw.([]any)
	if !ok {
		return nil, fmt.Errorf("invalid %s: expected an array", option.MCP)
	}
	if len(items) > maxListLen {
		return nil, fmt.Errorf("invalid %s: at most %d values", option.MCP, maxListLen)
	}
	out := make([]string, 0, len(items))
	for _, item := range items {
		text, isText := item.(string)
		if !isText {
			return nil, fmt.Errorf("invalid %s: expected an array of strings", option.MCP)
		}
		out = append(out, text)
	}
	return out, nil
}

// FromMessage reads the set fields of a generated options message (proto
// field name = option name), checked against their options on `surface`.
func (d *Document) FromMessage(message protoreflect.Message, surface string) (Values, error) {
	values := Values{}
	var failure error
	message.Range(func(field protoreflect.FieldDescriptor, value protoreflect.Value) bool {
		option, ok := d.Option(string(field.Name()))
		if !ok {
			failure = fmt.Errorf("scoreopts: %s is not a generated option", field.FullName())
			return false
		}
		converted := protoValue(field, value)
		if err := option.Check(surface, converted); err != nil {
			failure = err
			return false
		}
		values[option.Name] = converted
		return true
	})
	if failure != nil {
		return nil, failure
	}
	return values, nil
}

// protoValue converts a field value: lists to []string, numbers widened.
func protoValue(field protoreflect.FieldDescriptor, value protoreflect.Value) any {
	if field.IsList() {
		list := value.List()
		out := make([]string, 0, list.Len())
		for i := 0; i < list.Len() && i < maxListLen+1; i++ {
			out = append(out, list.Get(i).String())
		}
		return out
	}
	switch field.Kind() {
	case protoreflect.BoolKind:
		return value.Bool()
	case protoreflect.StringKind:
		return value.String()
	case protoreflect.DoubleKind, protoreflect.FloatKind:
		return value.Float()
	case protoreflect.Uint32Kind, protoreflect.Uint64Kind, protoreflect.Fixed32Kind, protoreflect.Fixed64Kind:
		return value.Uint()
	default:
		return value.Int()
	}
}
