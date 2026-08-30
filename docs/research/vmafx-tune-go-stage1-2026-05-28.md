<!-- markdownlint-disable MD013 -->
# Research: vmafx-tune Go port — Stage 1 design decisions

**Date**: 2026-05-28
**Author**: ADR-0705 implementation pass

## Summary

Stage 1 ports the Python `vmaf-tune compare` subcommand to Go. Three design
questions needed research: (1) subprocess vs. CGo for encoding/scoring, (2) the
concurrency model, (3) JSON schema strategy for downstream compatibility.

## Subprocess vs. CGo for encoding and scoring

**Decision**: subprocess ffmpeg + vmaf binary for Stage 1.

The alternative was linking libavcodec via CGo for encoding and using the libvmaf
C API for scoring. That approach would eliminate the process-spawn overhead on
short clips but introduces:

- A C CGo build dependency that prevents `GOARCH=amd64 GOOS=linux` static binary
  cross-compilation from non-Linux hosts.
- A libvmaf version lock: the binary would need to be compiled against a specific
  libvmaf.so version, complicating distribution.
- Significantly more build system complexity (`meson` for the C library, `go build`
  for the Go layer, a bridge shim for each encoder).

For the rate-quality tuning workload, encode wall time (seconds to minutes per CRF
probe) completely dominates the subprocess spawn overhead (< 10 ms). The subprocess
model is chosen for Stage 1; a CGo scorer path for libvmaf is a Stage-2 option if
profiling shows the spawn overhead is measurable.

## Concurrency model

The Python implementation uses `ThreadPoolExecutor` with a decode semaphore
(`ADR-0577`). Stage 1 uses `sync.WaitGroup` with one goroutine per `(codec,
target)` pair and no concurrency cap. The Python semaphore exists specifically
because reference-YUV decodes write multi-gigabyte temp files and the semaphore
prevents disk exhaustion. Since Stage-1 scores via the vmaf binary directly against
the source container (no pre-decode YUV step), the concurrent-decode pressure is
absent. Stage-2 will add `--workers` / `--max-concurrent-decodes` when the
pre-decoded-ref optimisation (ADR-0607) is ported to Go.

## JSON schema strategy

The Python `compare.py` emits schema-v1 (single target) and schema-v2 (multi-target
sweep) JSON payloads. The existing Python `report.py` renderer and downstream tooling
that reads `vmaf-tune` output ingest these schemas. Three options were considered:

1. **Emit a new Go-native schema** — breaks downstream tooling immediately.
2. **Emit Python-compatible schema with NaN → null coercion** — chosen. Mirrors the
   Python `_nan_to_none()` discipline (Bug #2, BBB e2e 2026-05-17). RFC 8259
   strict output works with `encoding/json` in Go (`json.Number` or `any`).
3. **Emit schema-v3 with explicit `source_language: go` flag** — premature; no
   downstream tooling needs to distinguish Go vs. Python output yet.

Option 2 is chosen. The `nanToNull` helper in `pkg/report/report.go` mirrors the
Python implementation exactly.

## Bisect algorithm fidelity

The bisect in `pkg/bisect/bisect.go` mirrors Python Phase B (`bisect.py`) with one
deliberate difference: Go's `(lo + hi + 1) / 2` midpoint rounds toward the high-CRF
(low-quality) end of the window, while Python's `(lo + hi) // 2` rounds toward the
low-CRF (high-quality) end. The Go rounding is the safer choice: it means the best-
so-far record is populated only after we have measured that CRF. Changing the bias
would require re-validating the convergence guarantees against real content. The
existing unit test (`TestBisect_convergence`) verifies the converged CRF for a
linear VMAF model.
