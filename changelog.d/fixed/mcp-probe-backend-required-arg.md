- MCP server: calling the `probe_backend` tool without the required
  `backend` argument now raises the uniform
  `tool 'probe_backend' missing required argument: 'backend'` `ValueError`
  (via the shared `_call_tool` KeyError→ValueError wrapper, matching every
  other required-arg tool) instead of a bespoke `'backend' is required for
  probe_backend` message. Restores green on the `MCP Smoke` CI lane
  (`test_call_tool_missing_backend_raises_value_error`).
