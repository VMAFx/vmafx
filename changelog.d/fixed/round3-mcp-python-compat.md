### Fixed

Round-3 bug-hunt — MCP server (Python) + compat harness (golden-safe; no golden assertions touched).

- **Non-ASCII MCP HTTP token → HTTP 500 (R3-12)** (`mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py`): `hmac.compare_digest` on two `str` operands raises `TypeError` when `VMAFX_MCP_HTTP_TOKEN` has a code point > 255, 500'ing every request (auth outage). Fix: compare UTF-8 bytes. Load-bearing test added.
- **Stale `tools_resource_path` (R3-20)** (`compat/python-vmaf/config.py`): returned the pre-ADR-0700 `python/vmaf/tools/resource/` path → `FileNotFoundError` for `Hanley_McNeil.mat` in `significanceHM()`. Fix: point at `compat/python-vmaf/tools/resource/`, mirroring `resource_path`.
- **`_ffprobe_geometry` KeyError vs documented ValueError (R3-22)** (`mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`): a video stream lacking width/height raised `KeyError`; fix uses `.get` + raises the documented `ValueError`. Load-bearing test added.
- R3-21 (mkdtemp leak in `_describe_worst_frames`) was already fixed on the live tree (`tempfile.TemporaryDirectory`) — no change.
