// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

// Command obsgen writes or checks the files pkg/observability/obsgen
// generates from the metric definition: the Grafana dashboards, the
// Prometheus rule file and its promtool test, the Helm chart's PrometheusRule
// template, dashboard copies and settings block, and
// docs/observability/metrics.md. Run it from the repository root:
//
//	go run ./tools/obsgen -write   # regenerate
//	go run ./tools/obsgen -check   # exit 1 when a committed file differs
//
// With -render-rules it prints the rule file for the default settings
// overlaid with the monitoring.slo, .burnRates and .alerts of each -values
// file in turn, as Helm merges values files; the chart's PrometheusRule
// rendered with the same files has the same groups:
//
//	go run ./tools/obsgen -render-rules -values my-values.yaml -out rules.yaml
package main

import (
	"bytes"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"github.com/VMAFx/vmafx/pkg/observability/obsgen"
)

// valueFiles collects the repeated -values flag.
type valueFiles []string

func (v *valueFiles) String() string     { return fmt.Sprint(*v) }
func (v *valueFiles) Set(p string) error { *v = append(*v, p); return nil }

func main() {
	write := flag.Bool("write", false, "write the generated files")
	check := flag.Bool("check", false, "fail when a committed file differs from the generated one")
	render := flag.Bool("render-rules", false, "print the rule file for the settings of the -values files")
	out := flag.String("out", "", "with -render-rules: write the rule file here instead of standard output")
	var values valueFiles
	flag.Var(&values, "values", "with -render-rules: a Helm values file to overlay (repeatable, later wins)")
	flag.Parse()
	var err error
	if *render {
		err = renderRules(values, *out)
	} else {
		err = run(*write, *check)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "obsgen:", err)
		os.Exit(1)
	}
}

// renderRules writes the rule file for the defaults overlaid with each
// values file.
func renderRules(values []string, out string) error {
	s := obsgen.DefaultSettings()
	for _, p := range values {
		doc, err := os.ReadFile(p) // #nosec G304 -- a values file the operator names
		if err != nil {
			return err
		}
		if s, err = s.ApplyValues(doc); err != nil {
			return fmt.Errorf("%s: %w", p, err)
		}
	}
	rules, err := obsgen.RenderRules(s)
	if err != nil {
		return err
	}
	if out == "" {
		_, err = os.Stdout.Write(rules)
		return err
	}
	return os.WriteFile(out, rules, 0o644) // #nosec G306 G703 -- a rule file Prometheus reads, at the path the operator gave
}

func run(write, check bool) error {
	if write == check {
		return errors.New("pass exactly one of -write and -check")
	}
	if _, err := os.Stat("go.mod"); err != nil {
		return errors.New("run from the repository root")
	}
	files, err := obsgen.Generate()
	if err != nil {
		return err
	}
	var stale []string
	for _, f := range files {
		changed, err := apply(f, write)
		if err != nil {
			return err
		}
		if changed {
			stale = append(stale, f.Path)
		}
	}
	if check && len(stale) > 0 {
		return fmt.Errorf("stale generated files %v; run go run ./tools/obsgen -write", stale)
	}
	return nil
}

// apply compares f with the file on disk, writes it when write is set, and
// reports whether the two differed.
func apply(f obsgen.File, write bool) (bool, error) {
	path := filepath.FromSlash(f.Path)
	old, err := os.ReadFile(path) // #nosec G304 -- a path obsgen generates, under the repository root
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return false, err
	}
	content, err := f.Merge(old)
	if err != nil {
		return false, err
	}
	if bytes.Equal(old, content) {
		return false, nil
	}
	if !write {
		return true, nil
	}
	// World-readable, as a checkout writes them: the Compose example mounts
	// the generated dashboards and provisioning into containers that run as
	// another user.
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil { // #nosec G301 -- repository directory, read by containers
		return false, err
	}
	return true, os.WriteFile(path, content, 0o644) // #nosec G306 G703 -- repository file, read by containers, at a path the generator built
}
