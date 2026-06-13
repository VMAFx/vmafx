# Research-0118: external-bench wrapper schema validation

- **Status**: Active
- **Workstream**: external benchmark harness robustness
- **Last updated**: 2026-05-14

## Question

Should `tools/external-bench/compare.py` continue trusting wrapper JSON
until aggregation, or validate each wrapper payload at the subprocess
boundary?

## Sources

- [`tools/external-bench/compare.py`](../../tools/external-bench/compare.py)
  parsed wrapper JSON and returned it directly to aggregation.
- [`tools/external-bench/README.md`](../../tools/external-bench/README.md)
  documents the wrapper schema as the licence-boundary contract.
- [`tools/external-bench/AGENTS.md`](../../tools/external-bench/AGENTS.md)
  marks that output schema as the load-bearing contract between
  wrapper scripts and `compare.py`.

## Findings

The wrapper seam is where schema errors should be reported. Without a
validator, a malformed wrapper output reaches `aggregate()` and fails
later as a generic `KeyError` or `TypeError`, losing the wrapper name,
the output file context, and the exact missing field. The existing main
loop already catches `RuntimeError` from `run_wrapper()` and skips the
bad `(competitor, corpus item)` pair, so moving schema checks into
`run_wrapper()` fits the existing failure path.

## Alternatives considered

Validating only in `aggregate()` was rejected because aggregation sees
only a list of payloads and no longer knows which wrapper invocation
produced the bad JSON.

Adding a JSON Schema dependency was rejected for this small fixed
contract. A typed in-tree validator keeps the harness dependency-free
and is enough to pin required keys, numeric fields, and
`summary.competitor` identity.

## Decision

Add `validate_wrapper_output()` and call it immediately after JSON
parsing in `run_wrapper()`. Malformed JSON raises a `RuntimeError`
with `invalid JSON`; schema violations raise a `RuntimeError` with
`invalid schema`. Extra fields remain allowed so wrappers can carry
debug metadata without changing the aggregation contract.
