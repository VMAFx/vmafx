// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

package benchmark_test

import (
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/benchmark"
	"github.com/VMAFx/vmafx/pkg/corpus"
)

// TestBenchmarkReadsWhatCorpusWrites round-trips a corpus row through the
// writer the `corpus` subcommand uses and the reader the `benchmark`
// subcommand uses. In Python one tool consumes the other's output directly.
func TestBenchmarkReadsWhatCorpusWrites(t *testing.T) {
	row := map[string]any{
		"src": "clip.y4m", "encoder": "libx264", "preset": "medium", "crf": 23,
		"vmaf_score": 93.5, "bitrate_kbps": 4200.25, "duration_s": 10.0,
		"framerate": 24.0, "exit_status": 0, "encode_time_ms": 1200.0,
		"score_time_ms": 800.0, "vmaf_model": "vmaf_v0.6.1",
		"width": 1920, "height": 1080, "pix_fmt": "yuv420p",
		"schema_version": 1, "run_id": "r1", "timestamp": "2026-08-30T00:00:00Z",
	}
	line, err := corpus.WriteRowLine(row)
	if err != nil {
		t.Fatalf("corpus.WriteRowLine: %v", err)
	}
	t.Logf("corpus wrote: %s", line)

	rows, err := benchmark.ParseCorpusJSONL(strings.NewReader(line))
	if err != nil {
		t.Fatalf("benchmark.ParseCorpusJSONL rejected the corpus writer's own output: %v", err)
	}
	if len(rows) != 1 {
		t.Fatalf("parsed %d rows, want 1", len(rows))
	}
	for _, k := range []string{"src", "encoder", "preset", "crf", "vmaf_score", "bitrate_kbps"} {
		if _, ok := rows[0][k]; !ok {
			t.Errorf("round-trip lost key %q", k)
		}
	}
	t.Logf("round-trip OK, %d keys survived", len(rows[0]))

	// The layer where "cannot read" would really bite: eligibility + summarise.
	sums, err := benchmark.Summarize(rows, 92.0, "")
	if err != nil {
		t.Fatalf("benchmark.Summarize on corpus-written rows: %v", err)
	}
	if len(sums) == 0 {
		t.Errorf("Summarize produced 0 summaries from a valid corpus row -- benchmark cannot consume corpus output")
	} else {
		out, rerr := benchmark.RenderCSV(sums)
		if rerr != nil {
			t.Fatalf("RenderCSV: %v", rerr)
		}
		t.Logf("summarised %d row(s); csv head: %.120s", len(sums), out)
	}
}
