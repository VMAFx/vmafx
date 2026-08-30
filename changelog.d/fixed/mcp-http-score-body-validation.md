# Fixed: MCP HTTP transport `POST /v1/score` body-validation edge cases (ADR-1075)

Two correctness bugs in the HTTP transport's score handler have been fixed:

- **Chunked oversize body now returns 413** (previously mis-reported as 400
  "invalid JSON"). `aiohttp.web.HTTPRequestEntityTooLarge` raised inside
  `request.json()` for chunked bodies exceeding the 4 MiB `client_max_size`
  limit was caught by the generic `except Exception` handler and re-wrapped
  as a 400 response. The fix adds a targeted `except HTTPRequestEntityTooLarge:
  raise` before the generic handler so aiohttp converts it to the correct 413
  wire response.

- **Non-object JSON bodies now return structured 400** (previously caused an
  uncaught `TypeError` → uncontrolled plain-text 500 with no `request_id`).
  Sending `null`, a JSON array, an integer, or any non-object JSON value as the
  request body resulted in a `TypeError: argument of type '...' is not iterable`
  on the field-membership test. The fix validates `isinstance(body, dict)` after
  parsing and returns a structured 400 with `request_id` when the check fails.

Nine regression tests added in `tests/test_mcp_http_edge_cases_adr1075.py`.
`docs/mcp/http-transport.md` updated to document 401 and 413 in the error table.
