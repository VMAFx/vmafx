<!-- markdownlint-disable MD013 MD060 -->
# Go orchestrator HISS cleanup — 2026-09-21

## Scope

The Go `recommend` command and corpus row orchestrator carried six baselined
HISS findings: five functions exceeded the 60-line complexity ceiling and one
temporary-encode removal result was discarded. The local-data-root migration
must touch these files, so the findings are fixed rather than exempted.

## Decision

The refactor keeps each existing seam and extracts one-purpose helpers:

| Area | Extracted responsibility | Preserved contract |
|---|---|---|
| `recommend` flags | input, search, and selection registration | identical Cobra names and defaults |
| Corpus selection | target validation and uncertainty rendering | identical JSON and human-readable output |
| Encode recommendation | validation, sweep collection, JSONL write, result rendering | identical visit order and predicates |
| Corpus iteration | one-time setup, per-cell encode/score, cleanup | identical row order and schema-v3 values |
| Row construction | base fields, canonical-six aggregates, encoder stats | Python-compatible field values and NaN policy |

The discarded `os.Remove` result is now handled. `os.ErrNotExist` remains a
successful cleanup because a runner may remove its own output; every other
failure stops the sweep before the row is emitted. Requested source hashing
also reports read failures instead of producing an empty provenance field.
Distorted-decode failure remains the documented parity fallback: it is logged
with its exit status and the unchanged request is scored so the failed cell is
recorded.

## Alternatives considered

No alternatives: this is a behavior-preserving decomposition plus explicit
handling of previously discarded errors. Suppressions, larger thresholds, or
baseline edits would retain the defects.

## Verification

```bash
TMPDIR=/home/kilian/.cache/vmafx-prepush-tmp \
  go test ./cmd/vmafx-tune/cmd ./pkg/corpus
standardsctl audit -touched cmd/vmafx-tune/cmd/recommend.go,pkg/corpus/corpus.go
```

The corpus regression suite includes a non-empty temporary-output directory
that proves cleanup failure is returned and no row is emitted. Existing tests
pin flag names, human and JSON recommendation output, row ordering, bitrate,
HDR/model selection, encode retention, and schema completeness.

## Delivery declarations

- no ADR needed: only-one-way warning and error-handling cleanup
- no rebase-sensitive invariants: both Go surfaces are fork-only and retain
  their existing documented contracts
- no FFmpeg patch impact: no libvmaf public surface changed
