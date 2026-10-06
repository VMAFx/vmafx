---
paths:
  - core/tools/cli_provenance.c
  - core/tools/cli_provenance.h
  - core/tools/vmaf.cpp
  - core/tools/cli_options.gen.inc
  - core/tools/cli_parse.cpp
invariant: Option table generated from core/api/vmafx.toml; provenance member from a C TU.
---
<!-- markdownlint-disable MD013 -->
# Generated option table and provenance receipt (ADR-2044)

- `cli_parse.cpp` includes `cli_options.gen.inc` (short string, `ARG_*`, `long_opts[]`, usage lines) inside its anon namespace; generated from option groups of `core/api/vmafx.toml`. New flag / alias / help text = definition entry + `python3 scripts/codegen/vmafx-api.py --write`, never an `.inc` edit. Existing spellings stay (HISS-14): `core/test/test_cli_option_table.cpp` frozen list.
- `--help` must keep `--no_cuda` / `--no_sycl` / `--no_hip` / `--no_metal` lines: both MCP servers probe `vmaf --help` for them.
- JSON receipt = `backend_used`, `feature_backends`, `provenance`. `provenance` from `cli_format_provenance_member()` (`cli_provenance.c`, C TU on purpose: C++ TU including generated C headers trips `modernize-use-using` / `performance-enum-size`). Keys = `VmafxProvenance` field names; server reads them with strict protojson (unknown key = request error). New struct field -> new key here + `test_cli_provenance`.
