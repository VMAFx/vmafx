// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

// Package encoderregistry reads the encoder registry generated from
// api/vmafx-encoders.toml (ADR-3388).
//
// registry.gen.json is written by scripts/codegen/vmafx-api.py; never edit it.
// The Python vmaf-tune adapters read a byte-identical copy, so pkg/encoder,
// pkg/codecadapter and vmaf-tune take each codec's quality knob, windows,
// presets and capability flags from one document instead of three hand-kept
// tables. Rows without an adapter table are the planned, listed and
// decision-gated gaps of the encoder matrix.
package encoderregistry

import (
	_ "embed" // registry.gen.json
	"encoding/json"
	"fmt"
	"sync"
)

//go:embed registry.gen.json
var generated []byte

// Adapter is the tuning contract of an implemented encoder row.
type Adapter struct {
	Knob                     string   `json:"knob"`
	QualityRange             [2]int   `json:"quality_range"`
	AbsoluteRange            [2]int   `json:"absolute_range"`
	QualityDefault           int      `json:"quality_default"`
	InvertQuality            bool     `json:"invert_quality"`
	Presets                  []string `json:"presets"`
	PresetStyle              string   `json:"preset_style"`
	Version                  string   `json:"version"`
	ProbePreset              string   `json:"probe_preset"`
	ProbeQuality             int      `json:"probe_quality"`
	SupportsQPFile           bool     `json:"supports_qpfile"`
	SupportsEncoderStats     bool     `json:"supports_encoder_stats"`
	SupportsTwoPass          bool     `json:"supports_two_pass"`
	TwoPassABRAtPass1Bitrate bool     `json:"two_pass_abr_at_pass1_bitrate"`
}

// Encoder is one row of the registry.
type Encoder struct {
	ID            string   `json:"id"`
	Family        string   `json:"family"`
	Paradigm      string   `json:"paradigm"`
	Class         string   `json:"class"`
	Status        string   `json:"status"`
	Interface     string   `json:"interface"`
	FFmpegEncoder string   `json:"ffmpeg_encoder"`
	Hardware      string   `json:"hardware"`
	LicenceClass  string   `json:"licence_class"`
	Maintenance   string   `json:"maintenance"`
	Upstream      string   `json:"upstream"`
	Checked       string   `json:"checked"`
	UseCases      []string `json:"use_cases"`
	Surfaces      []string `json:"surfaces"`
	Adapter       *Adapter `json:"adapter"`
	Notes         string   `json:"notes"`
}

// Variant is one ADAPTER@VARIANT row (ADR-0644).
type Variant struct {
	Adapter      string `json:"adapter"`
	Variant      string `json:"variant"`
	Kind         string `json:"kind"`
	Status       string `json:"status"`
	LicenceClass string `json:"licence_class"`
	Upstream     string `json:"upstream"`
	Checked      string `json:"checked"`
	Notes        string `json:"notes"`
}

// Document is registry.gen.json.
type Document struct {
	Encoders []Encoder `json:"encoders"`
	Variants []Variant `json:"variants"`
}

// StatusImplemented marks a row with an adapter table that runs today.
const StatusImplemented = "implemented"

var load = sync.OnceValues(func() (*Document, error) {
	return parse(generated)
})

func parse(raw []byte) (*Document, error) {
	var doc Document
	if err := json.Unmarshal(raw, &doc); err != nil {
		return nil, fmt.Errorf("encoderregistry: registry.gen.json: %w", err)
	}
	for i := range doc.Encoders {
		row := &doc.Encoders[i]
		if (row.Status == StatusImplemented) != (row.Adapter != nil) {
			return nil, fmt.Errorf("encoderregistry: row %q: an adapter table is set "+
				"exactly when status is %s", row.ID, StatusImplemented)
		}
	}
	return &doc, nil
}

// Load returns the parsed registry.
func Load() (*Document, error) {
	return load()
}

// Implemented returns the rows that carry an adapter table, in registry order.
func Implemented() ([]Encoder, error) {
	doc, err := load()
	if err != nil {
		return nil, err
	}
	out := make([]Encoder, 0, len(doc.Encoders))
	for _, row := range doc.Encoders {
		if row.Adapter != nil {
			out = append(out, row)
		}
	}
	return out, nil
}

// Lookup returns the row with the given id.
func Lookup(id string) (Encoder, error) {
	doc, err := load()
	if err != nil {
		return Encoder{}, err
	}
	for _, row := range doc.Encoders {
		if row.ID == id {
			return row, nil
		}
	}
	return Encoder{}, fmt.Errorf("encoderregistry: no encoder row %q", id)
}
