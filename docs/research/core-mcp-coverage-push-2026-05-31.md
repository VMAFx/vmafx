<!-- markdownlint-disable MD060 -->
# Core MCP transport coverage push — 2026-05-31

ADR-0108 §1 research digest for the
`test/core-mcp-coverage-push` branch.

## Scope

Lift line coverage on `core/src/mcp/` by adding a focused C test file
(`core/test/test_mcp_coverage.c`) that exercises the dispatcher error
envelopes, the lifecycle state-machine edges, and the transport-level
error paths that the existing `test_mcp_smoke.c` does not reach.

No production code is changed.

## Pre-state

`core/src/mcp/` carries one C test (`core/test/test_mcp_smoke.c`,
~670 LOC) wired into the build under `enable_mcp` with the `slow`
suite tag. That file pins the **happy-path** contract: init /
start_stdio / close lifecycle, `tools/list` round-trip, `tools/call`
for `list_features`, method-not-found error envelope, UDS round-trip,
`compute_vmaf` real-score against the testdata 576x324 8-bit YUV pair,
`compute_vmaf` 10-bit ramp, and the SSE `GET /mcp/sse` + `POST /mcp/sse`
inline round-trip.

What it does not reach (baseline gcov, CPU-only build, all transports
enabled):

| TU                                    | smoke-only line cov |
| ------------------------------------- | ------------------: |
| `core/src/mcp/dispatcher.c`           |             57.74 % |
| `core/src/mcp/mcp.c`                  |             60.00 % |
| `core/src/mcp/transport_stdio.c`      |             65.17 % |
| `core/src/mcp/transport_sse.c`        |             58.10 % |
| `core/src/mcp/transport_uds.c`        |             65.98 % |
| `core/src/mcp/compute_vmaf.c`         |             67.45 % |
| `core/src/mcp/3rdparty/cJSON/cJSON.c` |             38.15 % |

The dispatcher number is the most worrying: ~42 % of `dispatcher.c`
lines never run under the smoke harness, including the entire JSON-RPC
error-envelope construction path that production clients land on the
moment they send a malformed request.

## Audit method

1. Cold-read every C file in `core/src/mcp/` plus
   `core/include/libvmaf/libvmaf_mcp.h`.
2. For each entry point and helper, classify branches into:
   - **happy path** (smoke already covers),
   - **input validation / error envelope** (likely uncovered),
   - **state-machine edge** (double-start, stop-then-close, …),
   - **transport-specific error path** (overflow line, 404, malformed
     HTTP request line, oversized POST body, …).
3. Stage a gcov measurement against the smoke harness alone to confirm
   the qualitative classification matches gcov line numbers. Use the
   delta as the success metric for the new file.
4. Add tests until the dispatcher line coverage passes a 75 % gate
   (production-grade for an error-envelope-heavy TU).

## What the new file covers (27 sub-tests)

### Dispatcher (`core/src/mcp/dispatcher.c`)

| Sub-test                                  | What it pins                                                    |
| ----------------------------------------- | --------------------------------------------------------------- |
| `test_initialize_roundtrip`               | `handle_initialize` → serverInfo + capabilities envelope        |
| `test_resources_list_roundtrip`           | `handle_resources_list` → empty `resources:[]`                  |
| `test_parse_error_envelope`               | `build_error_response` for JSONRPC_PARSE_ERROR (-32700)         |
| `test_invalid_request_missing_method`     | Top-level `cJSON_IsString(method)` check → -32600               |
| `test_invalid_request_non_string_method`  | Same gate, integer-typed `method`                               |
| `test_notification_no_response`           | `is_notification` branch → response_out=NULL, no write          |
| `test_tools_call_missing_params`          | `handle_tools_call` → -32602 on `!cJSON_IsObject(params)`       |
| `test_tools_call_non_string_name`         | Same handler, non-string `name`                                 |
| `test_tools_call_unknown_tool`            | Loop-fall-through → JSONRPC_METHOD_NOT_FOUND (-32601)           |
| `test_compute_vmaf_missing_arguments`     | Tool's `set_err` propagation into `error.message`               |
| `test_compute_vmaf_missing_required_field`| Same, distinct error string (parse_arguments → set_err)         |

### Lifecycle (`core/src/mcp/mcp.c`)

| Sub-test                                            | What it pins                                            |
| --------------------------------------------------- | ------------------------------------------------------- |
| `test_init_rejects_non_power_of_two_queue_depth`    | `validate_config` power-of-2 gate                       |
| `test_init_rejects_max_drain_over_cap`              | `validate_config` 64-cap on `max_drain_per_frame`       |
| `test_init_accepts_valid_power_of_two_queue_depth`  | Positive path through the same validator                |
| `test_user_agent_surfaces_in_initialize`            | Dup-and-route through to `serverInfo.name`              |
| `test_transport_available_positive_and_oob`         | Bit-test path + early-return on id > 31                 |
| `test_start_stdio_double_start_returns_ebusy`       | `atomic_compare_exchange_strong` loser branch → -EBUSY  |
| `test_start_uds_rejects_empty_path`                 | `path_len == 0u` rejection                              |
| `test_start_uds_rejects_overlong_path`              | `path_len >= 100u` rejection (sun_path overflow guard)  |
| `test_start_sse_rejects_empty_path`                 | SSE path length-0 rejection                             |
| `test_start_sse_rejects_overlong_path`              | SSE path length > 256 B rejection                       |
| `test_close_after_eof_joins_worker`                 | Canonical EOF-then-close lifecycle                      |

### stdio transport (`core/src/mcp/transport_stdio.c`)

| Sub-test                                       | What it pins                                                                    |
| ---------------------------------------------- | ------------------------------------------------------------------------------- |
| `test_stdio_oversize_line_overflow_envelope`   | `read_line` returns -2 → canned `64 KiB line limit` envelope, drain to LF, resume |

### SSE transport (`core/src/mcp/transport_sse.c`)

| Sub-test                                | What it pins                                              |
| --------------------------------------- | --------------------------------------------------------- |
| `test_sse_health_endpoint`              | `GET /` → 200 OK with `{"server":"vmaf-mcp",…}` body      |
| `test_sse_404_unknown_path`             | URL match against `configured_path` failure → 404         |
| `test_sse_malformed_request_line`       | `sse_parse_request_line` failure → 400                    |
| `test_sse_post_without_content_length`  | `content_length <= 0` branch in `sse_serve_post` → 400    |

## Post-state

Coverage delta:

| TU                                    | smoke only | smoke + new | delta     |
| ------------------------------------- | ---------: | ----------: | --------: |
| `core/src/mcp/dispatcher.c`           |    57.74 % |     77.41 % | +19.67 pp |
| `core/src/mcp/mcp.c`                  |    60.00 % |     70.98 % | +10.98 pp |
| `core/src/mcp/transport_stdio.c`      |    65.17 % |     73.03 % |  +7.86 pp |
| `core/src/mcp/compute_vmaf.c`         |    67.45 % |     73.11 % |  +5.66 pp |
| `core/src/mcp/transport_sse.c`        |    58.10 % |     61.66 % |  +3.56 pp |
| `core/src/mcp/transport_uds.c`        |    65.98 % |     65.98 % |   +0.00 pp |
| `core/src/mcp/3rdparty/cJSON/cJSON.c` |    38.15 % |     40.37 % |  +2.22 pp |

`transport_uds.c` is unchanged: its happy-path round-trip (the only
non-error path that doesn't share code with the stdio worker) is
already covered by `test_mcp_smoke.c::test_uds_roundtrip`. The
remaining uncovered UDS lines are inside `serve_client`'s
oversize-line branch, which mirrors stdio's overflow path verbatim;
duplicating the test for UDS would be churn for no incremental
confidence. Flagged for a follow-up sweep, not a blocker here.

`transport_sse.c` ticks up modestly (+3.56 pp): four new HTTP
error-path tests but the bulk of uncovered lines are inside
`sse_emit_event` and `sse_extract_id` — both marked `__attribute__
((unused))` and reserved for the v4 broadcast pattern. Not test-able
through the public API in v3.

## Decision matrix considered

| Option                                                              | Pro                                                                | Con                                                              | Picked |
| ------------------------------------------------------------------- | ------------------------------------------------------------------ | ---------------------------------------------------------------- | ------ |
| New sibling file `test_mcp_coverage.c` (chosen)                     | Mirrors test_hip_smoke / test_metal_smoke split pattern; clean diff | Duplicates a small amount of harness boilerplate (~50 LOC)       | yes    |
| Extend `test_mcp_smoke.c` in place                                  | Single file, no boilerplate duplication                            | Doubles file LOC; review hard; tests already split into category | no     |
| Per-TU files (`test_dispatcher.c` / `test_mcp_transport.c` / …)     | Crisp blast radius                                                 | 3-4 meson targets to wire, more CI surface, no other test does it | no     |
| Wait until v4 SSE broadcast and bundle coverage with that feature   | Single PR, less moving                                             | Coverage gap stays in master for weeks; v4 ETA unknown            | no     |

## Reproducer

CPU-only build with all MCP transports enabled, then build + run both
MCP test executables:

```bash
cd core
meson setup build-mcp-coverage \
    -Denable_cuda=false -Denable_sycl=false -Denable_hip=false \
    -Denable_mcp=true \
    -Denable_mcp_stdio=true -Denable_mcp_uds=true -Denable_mcp_sse=enabled \
    -Denable_tests=true --buildtype=debug
ninja -C build-mcp-coverage
meson test -C build-mcp-coverage test_mcp_smoke test_mcp_coverage
```

Optional coverage measurement:

```bash
meson setup build-cov \
    -Denable_cuda=false -Denable_sycl=false -Denable_hip=false \
    -Denable_mcp=true \
    -Denable_mcp_stdio=true -Denable_mcp_uds=true -Denable_mcp_sse=enabled \
    -Denable_tests=true --buildtype=debug -Db_coverage=true
ninja -C build-cov
./build-cov/test/test_mcp_smoke
./build-cov/test/test_mcp_coverage
cd build-cov/src/libvmaf.so.3.0.0.p
for f in mcp_*.gcda; do
    base=$(basename $f .c.gcda | sed 's/^mcp_//')
    cov=$(gcov $f 2>&1 | grep -oP '\d+\.\d+(?=%)' | head -1)
    echo "$base: $cov%"
done
```

## Known gaps deferred

1. UDS oversize-line overflow path — mirrors stdio verbatim; deferred
   to a sweep PR that consolidates the read_line helpers in
   transport_stdio.c + transport_uds.c into one shared TU.
2. SSE v4 broadcast pattern (`sse_emit_event`, `sse_extract_id`) —
   currently dead code, marked `__attribute__((unused))`; not test-able
   through the v3 public API.
3. `compute_vmaf.c` error branches inside `score_yuv_pair` (vmaf_init
   OOM, vmaf_model_load OOM, picture_alloc OOM) — fault-injection-style
   tests that require either malloc-poisoning or rebuilding with a
   shim. Deferred to a focused OOM-injection PR.
4. `cJSON.c` parser error branches — out of scope; cJSON ships its
   own test suite, the libvmaf coverage push targets fork-added code.
5. The pre-existing `vmaf_mcp_stop()` double-call bug (running flag
   set to 2 unconditionally on every entry → re-enters the join branch
   for prev==2 on the third call, joining an already-joined thread).
   Observed locally while staging the test; a focused fix PR will land
   separately so this coverage PR stays purely additive.

## References

- `core/src/mcp/dispatcher.c`
- `core/src/mcp/mcp.c`
- `core/src/mcp/transport_stdio.c`
- `core/src/mcp/transport_uds.c`
- `core/src/mcp/transport_sse.c`
- `core/src/mcp/compute_vmaf.c`
- `core/test/test_mcp_smoke.c`
- `core/include/libvmaf/libvmaf_mcp.h`
- ADR-0108 — deep-dive deliverables rule
- ADR-0209 — embedded MCP scaffold
- ADR-0128 — MCP design / operational guardrails
- ADR-0332 — MCP SSE transport (mongoose replacement, plain POSIX)
