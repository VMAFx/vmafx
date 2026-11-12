---
paths:
  - core/src/pdjson.c
  - core/src/mcp/3rdparty/cJSON/cJSON.c
  - core/src/mcp/compute_vmaf.c
  - core/test/test_mcp_smoke.c
invariant: PDJSON_STACK_MAX=512, banned-function-free cJSON Semgrep guard, smoke tests, and path allowlist parity.
---
# pdjson depth, vendored cJSON, smoke test, and path allowlist

## Invariant: pdjson depth limit (ADR-1061)

`PDJSON_STACK_MAX` is defined to `512` at top of
`core/src/pdjson.c` (before `#ifdef` guard). This definition
must survive any future vendor sync. If replacing `pdjson.c` with
newer upstream release, check `PDJSON_STACK_MAX` either
defined by build system or re-added at top of file.
512 levels is well beyond any VMAF model or MCP message depth.

## Invariant: cJSON banned-function-free (ADR-0683 / ADR-1061)

`3rdparty/cJSON/cJSON.c` stays free of `sprintf`, `strcpy`, `strcat`,
`strtok`, `atoi`, `atof`, `gets`, `rand`, `system`. Enforced, not
remembered: `vmaf-no-strcpy-strcat-sprintf` Semgrep rule covers this
directory (hook `semgrep-local`, CI job `Semgrep`), and
`scripts/ci/tests/test_semgrep_vendored_scope.py` (hook
`test-semgrep-vendored-scope`) fails if path exclude or `.semgrepignore`
line ever hides it again -> that is what let the 1.7.19 re-vendor bring
eleven banned calls back unnoticed. Verify:

```bash
python3 -m unittest discover -s scripts/ci/tests -p test_semgrep_vendored_scope.py
```

Future cJSON version sync re-applies fork delta listed in
[`3rdparty/cJSON/AGENTS.md`](../3rdparty/cJSON/AGENTS.md). Never answer a
finding here with an exclusion.

## Smoke test

```text
build/test/test_mcp_smoke   # expects 18 tests run, 18 passed
```

v3 sub-test `test_sse_event_stream` spawns SSE server on
ephemeral loopback port, performs `GET /mcp/sse`, checks
for `Content-Type: text/event-stream`, `event: ready` field,
`data:` field, and blank-line frame terminator (per WHATWG
SSE §9.2, accessed 2026-05-09); then performs `POST /mcp/sse`
with `tools/list` JSON-RPC request, verifies inline
response contains `list_features`. v2 sub-tests
`test_uds_roundtrip` and `test_compute_vmaf_real_score` remain.

## Path allowlist parity (R2-4)

`compute_vmaf.c` `validate_path()` / `build_allowed_roots()` MUST keep same
allowlisted root set as Python (`_allowed_roots`) and Go (`AllowedRoots`)
MCP servers; change all three together. Guarded by
`test_mcp_compute_vmaf_allowlist.c`.
