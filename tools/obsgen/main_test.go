// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/observability/obsgen"
)

func writeValues(t *testing.T, dir, name, body string) string {
	t.Helper()
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

// TestRenderRulesOverlaysValuesInOrder: later files win key by key, the
// result is the rule file RenderRules writes for the merged settings.
func TestRenderRulesOverlaysValuesInOrder(t *testing.T) {
	dir := t.TempDir()
	first := writeValues(t, dir, "a.yaml", "monitoring: {slo: {jobSuccess: 0.9}, alerts: {queueAgeSeconds: 60}}\n")
	second := writeValues(t, dir, "b.yaml", "monitoring: {slo: {jobSuccess: 0.95}}\n")
	out := filepath.Join(dir, "rules.yaml")
	if err := renderRules([]string{first, second}, out); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	s := obsgen.DefaultSettings()
	s.SLO.JobSuccess = 0.95
	s.Alerts.QueueAgeSeconds = 60
	want, err := obsgen.RenderRules(s)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != string(want) {
		t.Errorf("rendered rules differ from RenderRules of the merged settings")
	}
}

// TestRenderRulesRefusesBadValues: an invalid value, an unknown key and a
// missing file are errors that name the file.
func TestRenderRulesRefusesBadValues(t *testing.T) {
	dir := t.TempDir()
	for name, body := range map[string]string{
		"objective.yaml": "monitoring: {slo: {jobSuccess: 1}}\n",
		"unknown.yaml":   "monitoring: {burnRates: {fast: {window: 1h}}}\n",
	} {
		p := writeValues(t, dir, name, body)
		err := renderRules([]string{p}, filepath.Join(dir, "out.yaml"))
		if err == nil || !strings.Contains(err.Error(), name) {
			t.Errorf("%s: err = %v, want an error naming the file", name, err)
		}
	}
	if err := renderRules([]string{filepath.Join(dir, "missing.yaml")}, ""); err == nil {
		t.Error("a missing values file was accepted")
	}
}

// TestApplyStaysUnderTheRoot: apply writes a generated path under the root it
// is given and refuses one that leaves it, writing nothing outside.
func TestApplyStaysUnderTheRoot(t *testing.T) {
	parent := t.TempDir()
	dir := filepath.Join(parent, "repo")
	if err := os.Mkdir(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	repo, err := os.OpenRoot(dir)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := repo.Close(); err != nil {
			t.Error(err)
		}
	})
	changed, err := apply(repo, obsgen.File{Path: "deploy/rules.yaml", Content: []byte("groups: []\n")}, true)
	if err != nil || !changed {
		t.Fatalf("apply inside the root = %v, %v", changed, err)
	}
	if got, err := os.ReadFile(filepath.Join(dir, "deploy", "rules.yaml")); err != nil || string(got) != "groups: []\n" {
		t.Errorf("written file = %q, %v", got, err)
	}
	if _, err := apply(repo, obsgen.File{Path: "../escape.yaml", Content: []byte("x\n")}, true); err == nil {
		t.Error("apply wrote a path outside the root")
	}
	if _, err := os.Stat(filepath.Join(parent, "escape.yaml")); !errors.Is(err, os.ErrNotExist) {
		t.Errorf("a file appeared outside the root: %v", err)
	}
}
