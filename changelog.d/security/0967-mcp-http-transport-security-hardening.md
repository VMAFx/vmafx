- **MCP HTTP transport hardened: auth + body limit + safer bind default (ADR-0967,
  Round 26 audit A.1)** — three security gaps in the `--transport http` mode are fixed:
  (1) Request bodies are now limited to 4 MiB via a Content-Length pre-flight middleware
  and `client_max_size` on the aiohttp application; unbounded POST bodies previously
  allowed memory exhaustion.  (2) Bearer token authentication is enforced by default:
  set `VMAFX_MCP_HTTP_TOKEN=<secret>` or set `VMAFX_MCP_HTTP_NO_AUTH=1` for
  gateway-protected deployments; without either, the server rejects all requests with
  HTTP 401 (fail-closed).  (3) The default bind host changes from `0.0.0.0` to
  `127.0.0.1`; set `VMAFX_MCP_HTTP_BIND=0.0.0.0` to restore all-interface binding.
  Optional TLS: set `VMAFX_MCP_HTTP_TLS_CERT` + `VMAFX_MCP_HTTP_TLS_KEY` to enable.
  **Breaking change**: any deployment relying on the `0.0.0.0` default must add
  `VMAFX_MCP_HTTP_BIND=0.0.0.0`.  The stdio/UDS transport is unaffected.
