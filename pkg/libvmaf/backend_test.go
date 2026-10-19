// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// pkg/libvmaf/backend_test.go — ScoreOnBackend passes the requested backend to
// the vmaf CLI, and Score leaves the CLI's own selection alone.

//go:build cgo

package libvmaf

import (
	"context"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/internal/execstub"
)

// recordArgs runs one score through a vmaf stub that records its argv and
// returns the recorded arguments.
func recordArgs(t *testing.T, backend string, explicit bool) []string {
	t.Helper()
	argsFile := filepath.Join(t.TempDir(), "vmaf-args.txt")
	t.Setenv("VMAF_ARGS_FILE", argsFile)
	scriptPath := filepath.Join(t.TempDir(), "vmaf")
	script := `#!/bin/sh
printf '%s\n' "$@" > "$VMAF_ARGS_FILE"
outfile=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o) outfile="$2"; shift 2 ;;
    *)  shift ;;
  esac
done
cat > "$outfile" <<'EOF'
` + goldenJSON + `
EOF
`
	execstub.Write(t, scriptPath, []byte(script))
	modelDir := t.TempDir()
	writeModel(t, modelDir, "vmaf_v0.6.1")
	s, err := New(scriptPath, modelDir)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if explicit {
		_, _, err = s.ScoreOnBackend(context.Background(), "ref.y4m", "dis.y4m", "vmaf_v0.6.1", backend)
	} else {
		_, _, err = s.Score(context.Background(), "ref.y4m", "dis.y4m", "vmaf_v0.6.1")
	}
	if err != nil {
		t.Fatalf("score: %v", err)
	}
	raw, err := os.ReadFile(argsFile)
	if err != nil {
		t.Fatalf("read recorded arguments: %v", err)
	}
	return strings.Split(strings.TrimSuffix(string(raw), "\n"), "\n")
}

// TestScoreOnBackend_PassesBackend: a named backend reaches the CLI as
// `--backend <name>` (positive).
func TestScoreOnBackend_PassesBackend(t *testing.T) {
	args := recordArgs(t, "cuda", true)
	i := slices.Index(args, "--backend")
	if i < 0 || i+1 >= len(args) || args[i+1] != "cuda" {
		t.Fatalf("want --backend cuda in argv, got %q", args)
	}
}

// TestScoreOnBackend_EmptyBackendAddsNoFlag: an empty backend keeps the CLI's
// own selection (boundary).
func TestScoreOnBackend_EmptyBackendAddsNoFlag(t *testing.T) {
	if args := recordArgs(t, "", true); slices.Contains(args, "--backend") {
		t.Fatalf("empty backend must not add --backend, got %q", args)
	}
}

// TestScore_AddsNoBackendFlag: the existing Score entry point is unchanged
// (negative: no backend flag appears).
func TestScore_AddsNoBackendFlag(t *testing.T) {
	if args := recordArgs(t, "", false); slices.Contains(args, "--backend") {
		t.Fatalf("Score must not add --backend, got %q", args)
	}
}
