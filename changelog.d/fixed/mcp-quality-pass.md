### MCP server quality pass

- Fixed `_list_extractors` scanning the wrong source directory (`libvmaf/src/feature`
  instead of `core/src/feature` — the path was stale from before the ADR-0700 rename),
  causing the tool to always return an empty list.
- Added `import logging` and a `vmaf_mcp` logger to `server.py`; `_call_tool` now logs
  INFO at entry and completion, and ERROR (with exception type) on any tool failure.
- Added `_dispatch_tool` inner function so the error-log wrapper and the
  ADR-0613 re-raise pattern are cleanly separated.
- Cleaned up `docs/mcp/tools.md`: removed the duplicate pre-ADR-0613 error-table
  section, removed the stale `list_backends` snippet duplication, and removed the
  dangling `ADR-0100` reference at the end of the progress-notifications section.
- Added `test_list_extractors_uses_core_src_path` regression test that asserts the
  `core/src/feature` directory is used and the old `libvmaf/src/feature` path does
  not exist.
