## Option groups generate every scoring surface (2026-10-06)

`rc4/api-wp8-options` (RC4 work package 8, ADR-2044). Fork-only files except
`core/tools/cli_parse.cpp` and `core/tools/vmaf.cpp`:

- `core/tools/cli_parse.cpp` no longer holds the short option string, the
  `ARG_*` enum, `long_opts[]` or the usage text: it includes the generated
  `core/tools/cli_options.gen.inc`. An upstream sync that adds a CLI option
  adds it to the option groups of `core/api/vmafx.toml` and regenerates
  (`python3 scripts/codegen/vmafx-api.py --write`); never edit the `.inc`.
  On the rebase onto master, master's `--check-sample-range` /
  `--check_sample_range` and `--list-backends` join the definition the same
  way, and `core/test/test_cli_option_table.cpp`'s frozen list gains them.
- `core/tools/vmaf.cpp`'s JSON receipt appends a `provenance` member
  (`cli_format_provenance_member()` of the C file `core/tools/cli_provenance.c`,
  so no C++ CLI source includes the generated C headers); keep it when the
  receipt code moves (WP5 moves it into the library's report writer).
- Generated files (`*.gen.*`, `proto/vmafx_api.proto`, `gen/go/**`, the
  marked regions of `docs/usage/cli.md`, `docs/usage/ffmpeg.md`,
  `docs/mcp/tools.md`, `docs/server/api-contract.md` and
  `api/openapi/vmafx-server-v1.yaml`): on a conflict take either side,
  regenerate, then `buf generate proto` and the oapi-codegen command of
  `gen/go/AGENTS.md`.
- Both MCP servers read `options.gen.json`; do not bring back the hand
  schemas (`scoringExtraProperties`, `_scoring_extra_properties`) or the
  hand flag lists of `scoreExtras.appendArgs` / `ScoreExtras.to_argv`.
