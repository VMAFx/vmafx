// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package encoderregistry

import "testing"

func TestLoadCommittedRegistry(t *testing.T) {
	t.Parallel()
	rows, err := Implemented()
	if err != nil {
		t.Fatalf("Implemented: %v", err)
	}
	if len(rows) == 0 {
		t.Fatal("no implemented rows in registry.gen.json")
	}
	row, err := Lookup("libx264")
	if err != nil {
		t.Fatalf("Lookup(libx264): %v", err)
	}
	if row.Adapter == nil || row.Adapter.Knob != "crf" {
		t.Errorf("libx264 adapter = %+v, want knob crf", row.Adapter)
	}
}

func TestLookupUnknown(t *testing.T) {
	t.Parallel()
	if _, err := Lookup("no-such-encoder"); err == nil {
		t.Error("Lookup accepted an unknown id")
	}
}

func TestParseRejects(t *testing.T) {
	t.Parallel()
	cases := map[string]string{
		"not json":                 `{`,
		"implemented, no adapter":  `{"encoders":[{"id":"x","status":"implemented"}]}`,
		"adapter on a planned row": `{"encoders":[{"id":"x","status":"planned","adapter":{}}]}`,
	}
	for name, raw := range cases {
		if _, err := parse([]byte(raw)); err == nil {
			t.Errorf("%s: parse accepted %s", name, raw)
		}
	}
}

func TestParseBoundaryEmpty(t *testing.T) {
	t.Parallel()
	doc, err := parse([]byte(`{"encoders":[],"variants":[]}`))
	if err != nil || len(doc.Encoders) != 0 {
		t.Fatalf("parse(empty) = %+v, %v", doc, err)
	}
}
