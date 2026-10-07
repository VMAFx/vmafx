// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

// Command obsgen writes or checks the files pkg/observability/obsgen
// generates from the metric definition: the Grafana dashboards under
// deploy/grafana/dashboards and docs/observability/metrics.md. Run it from the
// repository root:
//
//	go run ./tools/obsgen -write   # regenerate
//	go run ./tools/obsgen -check   # exit 1 when a committed file differs
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

func main() {
	write := flag.Bool("write", false, "write the generated files")
	check := flag.Bool("check", false, "fail when a committed file differs from the generated one")
	flag.Parse()
	if err := run(*write, *check); err != nil {
		fmt.Fprintln(os.Stderr, "obsgen:", err)
		os.Exit(1)
	}
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
	if bytes.Equal(old, f.Content) {
		return false, nil
	}
	if !write {
		return true, nil
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
		return false, err
	}
	return true, os.WriteFile(path, f.Content, 0o600)
}
