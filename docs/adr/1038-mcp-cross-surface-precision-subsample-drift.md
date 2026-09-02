<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1038: MCP cross-surface precision-default and probe-precision drift

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `mcp`, `correctness`, `cross-surface`

## Context

Both MCP surfaces (Go `cmd/vmafx-mcp` and Python `mcp-server/vmaf-mcp`) had two
related precision drift issues:

**Probe precision `"6"` resolves to `%.6g` (significant figures), not `%.6f`.**
`probe_backend` in both surfaces hardcoded `--precision 6`. The C CLI interprets
bare-integer precision strings as `%.Ng` format (significant figures, not decimal
places). The C CLI default is `--precision legacy` which maps to `%.6f`. A score
of 93.456789 prints as 93.4568 under `%.6g` but 93.456789 under `%.6f`. The probe
is only used for latency measurement, but a discrepancy between probe output and
normal scoring output was confusing.

**Default precision `"17"` (lossless IEEE-754) diverges from C CLI default `%.6f`.**
`ScoreRequest.precision` in the Python server defaulted to `"17"` (→ `%.17g`,
lossless IEEE-754 round-trip). The Go tools schema also defaulted to `"17"`. The
C CLI default is `%.6f` per ADR-0119. MCP callers who omit `precision` get lossless
output by default, which is not Netflix-compatible and diverges from direct CLI
invocations. Users comparing CLI output with MCP output would see different decimal
precision unless they knew to pass `precision=legacy` explicitly.

## Decision

Change all precision defaults to `"legacy"` (→ `%.6f`):

1. Go `impl.go`: `strArg(args, "precision", "17")` → `"legacy"` (two call sites:
   `handleVmafScore` and `handleVmafScoreEncoded`)
2. Go `impl.go`: hardcoded `"17"` in the backend self-test → `"legacy"`
3. Go `impl.go`: `--precision "6"` in `probeBackendScore` → `"legacy"`
4. Go `tools.go`: both `vmaf_score` and `vmaf_score_encoded` schema defaults → `"legacy"`
5. Python `server.py`: `ScoreRequest.precision` default → `"legacy"` (two class uses)
6. Python `server.py`: two tool schema `"default": "17"` → `"legacy"`
7. Python `server.py`: two `arguments.get("precision", "17")` dispatch fallbacks → `"legacy"`
8. Python `server.py`: `--precision "6"` in `_probe_backend` argv → `"legacy"`

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `"17"` as default, document divergence | Lossless by default | Not Netflix-compatible; diverges from direct CLI | Not acceptable for a drop-in CLI wrapper |
| Change to `"6"` (%.6g) | Shorter numbers | Not the same as `%.6f`; further drift from CLI | Wrong format |
| Expose separate `precision_mode` flag | Explicit disambiguation | Additional API surface; unnecessary | Over-engineered |

## Consequences

- **Positive**: MCP output now matches direct CLI invocation by default; Netflix
  golden-gate compatibility restored; probe output format consistent with scoring output.
- **Negative**: Callers who depended on lossless output by default must now pass
  `precision=max` explicitly.
- **Neutral / follow-ups**: The `subsample` omission from `handleVmafScoreEncoded`
  (Go) and the `vmaf_score` Python schema are tracked as separate follow-up items
  in the r7 MEDIUM findings.

## References

- ADR-0119: `--precision` flag semantics (`legacy` = `%.6f`, `max` = `%.17g`)
- r7 review findings: [r7-cross-surface-default-drift] probe `--precision 6`; precision default `"17"` in both MCP surfaces
