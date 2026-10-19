// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

// Package scoreopts reads the scoring options generated from the option groups
// of core/api/vmafx.toml (RC4 WP8, ADR-1852) and turns option values into `vmaf`
// command-line arguments.
//
// options.gen.json is written by scripts/codegen/vmafx-api.py; never edit it.
// It carries the MCP tool input schemas (JSON Schema 2020-12), every option a
// server reads (type, bounds, defaults, surfaces), the argument-vector spec and
// the flag of every command-line option. The Go MCP server serves the schemas
// as they are and both it and vmafx-server build their `vmaf` argument vectors
// here, so neither spells a scoring flag by hand.
package scoreopts

import (
	_ "embed" // options.gen.json
	"encoding/json"
	"fmt"
	"slices"
	"sync"
)

//go:embed options.gen.json
var generated []byte

// Schema is the subset of a JSON Schema the servers check values against.
type Schema struct {
	Type        string   `json:"type"`
	Enum        []any    `json:"enum"`
	Minimum     *float64 `json:"minimum"`
	Maximum     *float64 `json:"maximum"`
	Items       *Schema  `json:"items"`
	UniqueItems bool     `json:"uniqueItems"`
}

// Option is one option a server reads.
type Option struct {
	Name            string         `json:"name"`
	Group           string         `json:"group"`
	Type            string         `json:"type"`
	Surfaces        []string       `json:"surfaces"`
	MCP             string         `json:"mcp"`
	ProtoField      int            `json:"proto_field"`
	Schema          Schema         `json:"schema"`
	Repeat          []string       `json:"repeat"`
	Default         any            `json:"default"`
	SurfaceDefaults map[string]any `json:"surface_defaults"`
	DefaultMacro    string         `json:"default_macro"`
	Reserved        string         `json:"reserved"`
}

// ArgvEntry says how one option becomes `vmaf` flags.
type ArgvEntry struct {
	Option string            `json:"option"`
	MCP    string            `json:"mcp"`
	Stage  string            `json:"stage"`
	Flag   string            `json:"flag"`
	Form   string            `json:"form"`
	Suffix string            `json:"suffix"`
	Values map[string]string `json:"values"`
}

// Document is options.gen.json.
type Document struct {
	Dialect string                     `json:"dialect"`
	Tools   map[string]json.RawMessage `json:"tools"`
	Options []Option                   `json:"options"`
	Argv    []ArgvEntry                `json:"argv"`
	CLI     map[string]string          `json:"cli"`
	// LibraryDefaults maps the C macro an option names as its default to the
	// macro's value, read from the public headers when the file was generated.
	LibraryDefaults map[string]string `json:"library_defaults"`
}

var (
	loadOnce sync.Once
	loaded   *Document
	errLoad  error
)

// Load returns the generated document, parsed once.
func Load() (*Document, error) {
	loadOnce.Do(func() {
		loaded, errLoad = Parse(generated)
	})
	return loaded, errLoad
}

// Parse reads a document in the options.gen.json format.
func Parse(raw []byte) (*Document, error) {
	var doc Document
	if err := json.Unmarshal(raw, &doc); err != nil {
		return nil, fmt.Errorf("scoreopts: parse options.gen.json: %w", err)
	}
	if len(doc.Options) == 0 || len(doc.Argv) == 0 || len(doc.Tools) == 0 {
		return nil, fmt.Errorf("scoreopts: options.gen.json has no options, argv spec or tools")
	}
	return &doc, nil
}

// Raw is the embedded options.gen.json, byte for byte.
func Raw() []byte {
	return generated
}

// ToolSchema is the generated input schema of an MCP tool.
func (d *Document) ToolSchema(tool string) (json.RawMessage, error) {
	schema, ok := d.Tools[tool]
	if !ok {
		return nil, fmt.Errorf("scoreopts: no generated schema for tool %q", tool)
	}
	return schema, nil
}

// Option returns the option named `name`.
func (d *Document) Option(name string) (*Option, bool) {
	for i := range d.Options {
		if d.Options[i].Name == name {
			return &d.Options[i], true
		}
	}
	return nil, false
}

// ByMCP returns the option an MCP argument spells.
func (d *Document) ByMCP(argument string) (*Option, bool) {
	for i := range d.Options {
		if d.Options[i].MCP != "" && d.Options[i].MCP == argument {
			return &d.Options[i], true
		}
	}
	return nil, false
}

// Entry returns the argument-vector entry of an option.
func (d *Document) Entry(option string) (*ArgvEntry, bool) {
	for i := range d.Argv {
		if d.Argv[i].Option == option {
			return &d.Argv[i], true
		}
	}
	return nil, false
}

// Flag is the `vmaf` flag of an option: its argument-vector flag, else its
// command-line spelling (short form when it has one).
func (d *Document) Flag(option string) (string, error) {
	if entry, ok := d.Entry(option); ok && entry.Flag != "" {
		return entry.Flag, nil
	}
	if flag, ok := d.CLI[option]; ok {
		return flag, nil
	}
	return "", fmt.Errorf("scoreopts: option %q has no vmaf flag", option)
}

// DefaultOn is the default a surface documents for an option (nil: none).
func (o *Option) DefaultOn(surface string) any {
	if value, ok := o.SurfaceDefaults[surface]; ok {
		return value
	}
	return o.Default
}

// On reports whether the option is on a surface.
func (o *Option) On(surface string) bool {
	return slices.Contains(o.Surfaces, surface)
}

// Repeats reports whether the option takes a list on a surface.
func (o *Option) Repeats(surface string) bool {
	return slices.Contains(o.Repeat, surface)
}

// LibraryDefault is the value of the library default an option names
// (default_macro), for example the default model version.
func (d *Document) LibraryDefault(option string) (string, error) {
	o, ok := d.Option(option)
	if !ok || o.DefaultMacro == "" {
		return "", fmt.Errorf("scoreopts: option %q has no library default", option)
	}
	value, ok := d.LibraryDefaults[o.DefaultMacro]
	if !ok || value == "" {
		return "", fmt.Errorf("scoreopts: %s has no value in options.gen.json", o.DefaultMacro)
	}
	return value, nil
}

// Allows reports whether `value` is one of the values an enumerated option
// takes.
func (d *Document) Allows(option, value string) bool {
	o, ok := d.Option(option)
	if !ok {
		return false
	}
	for _, allowed := range o.Schema.Enum {
		if text, isText := allowed.(string); isText && text == value {
			return true
		}
	}
	return false
}
