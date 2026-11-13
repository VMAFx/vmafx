---
paths:
  - pkg/pershot/*.go
  - cmd/vmafx-tune/cmd/pershot*.go
invariant: Alphabetical wire fields + pyFloat for byte-identical plan JSON; unported flags fail fast.
---
<!-- markdownlint-disable MD013 MD029 -->
# Per-shot plan JSON byte parity and unported flags

## Rebase-sensitive invariants

13. **Per-shot plan JSON byte-compatible, not merely schema-compatible**
    (`pkg/pershot/plan_json.go`): Python emitter =
    `json.dumps(plan_doc, indent=2, sort_keys=True)`. Two consequences
    load-bearing, easy to break by accident. (a) Every wire struct
    (`planWire`, `shotWire`) declares fields in **alphabetical JSON-key
    order** — reproduces `sort_keys=True`. Reordering them for
    readability silently breaks byte-parity. (b) Floats go through `pyFloat`,
    restoring Python `repr()` form (`24.0`, not Go's `24`) and its
    fixed-vs-exponent threshold; `ensureASCII` reproduces
    `ensure_ascii=True`, `SetEscapeHTML(false)` stops Go escaping
    `< > &` where Python does not. Diff harness =
    `TestRenderPlanJSON_GoldenMatchesPython`; change failing it =
    regression, not formatting preference. ADR-0531 fixes
    NaN-to-`null` mapping for `bitrate_kbps`.

17. **`--predicate-module` and `--fast-nr` fail fast, are not ignored**
    (`cmd/vmafx-tune/cmd/pershot.go` `rejectUnportedPerShotFlags`): both flags
    registered so CLI surface matches Python parser, but invoking
    either returns error naming Python fallback. Accepting, ignoring
    them would silently change run's semantics (custom predicate would be
    replaced by bisect; NR early-elimination would not happen). If
    ONNX Go binding lands, `--fast-nr` graduates here first.
