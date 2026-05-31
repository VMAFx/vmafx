## Added

- **`core/test/test_mcp_coverage.c`** — coverage push for the embedded
  MCP server under `core/src/mcp/`. Complements
  `core/test/test_mcp_smoke.c` (which pins the happy-path round-trips)
  by covering the dispatcher error envelopes, lifecycle state-machine
  edges, and transport-level error paths the smoke test does not reach.
  Wired into the build under `enable_mcp` in `core/test/meson.build`
  alongside `test_mcp_smoke`; runs in the `slow` suite with the same
  60 s timeout.

  Coverage delta (gcov, CPU-only build, all transports enabled):

  | TU                                    | smoke only | smoke + new | delta     |
  | ------------------------------------- | ---------: | ----------: | --------: |
  | `core/src/mcp/dispatcher.c`           |    57.74 % |     77.41 % | +19.67 pp |
  | `core/src/mcp/mcp.c`                  |    60.00 % |     70.98 % | +10.98 pp |
  | `core/src/mcp/transport_stdio.c`      |    65.17 % |     73.03 % |  +7.86 pp |
  | `core/src/mcp/compute_vmaf.c`         |    67.45 % |     73.11 % |  +5.66 pp |
  | `core/src/mcp/transport_sse.c`        |    58.10 % |     61.66 % |  +3.56 pp |
  | `core/src/mcp/3rdparty/cJSON/cJSON.c` |    38.15 % |     40.37 % |  +2.22 pp |

  Surfaces newly covered (27 new sub-tests):

  - **Dispatcher** (`dispatcher.c`): `initialize` handshake,
    `resources/list` empty response, JSON-RPC parse-error envelope
    (`-32700`), invalid-request envelope (`-32600`) on missing /
    non-string `method`, notification swallow path (no id), `tools/call`
    `-32602` on missing-params and non-string-name, `-32601` on unknown
    tool, `compute_vmaf` `-32602` on missing arguments and
    missing-required-field — including the tool-supplied error message
    routed through `error.message`.
  - **Lifecycle** (`mcp.c`): `validate_config` rejection of non-power-of-2
    `queue_depth` and over-cap `max_drain_per_frame`, acceptance of valid
    power-of-2 + sub-cap, `user_agent` dup-and-surface via the
    `initialize` `serverInfo.name`, `vmaf_mcp_transport_available`
    positive returns and out-of-range (`> 31`) bounds-check,
    `vmaf_mcp_start_stdio` double-start `-EBUSY`, `start_uds` empty +
    over-long path rejection, `start_sse` empty + over-long path
    rejection, close-after-EOF lifecycle.
  - **stdio transport** (`transport_stdio.c`): oversize-line (> 64 KiB)
    parse-error overflow envelope + drain-and-resume.
  - **SSE transport** (`transport_sse.c`): health endpoint (`GET /`),
    404 on unknown path, 400 on malformed request line, 400 on POST
    without `Content-Length`.

  No production code changes — pure test-coverage PR. Companion digest:
  `docs/research/core-mcp-coverage-push-2026-05-31.md`.
