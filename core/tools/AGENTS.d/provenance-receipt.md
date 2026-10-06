---
paths:
  - core/tools/cli_provenance.c
  - core/tools/cli_provenance.h
  - core/tools/vmaf.cpp
  - core/tools/cli_options.gen.inc
  - core/tools/cli_parse.cpp
invariant: Option table generated from core/api/vmafx.toml; library writes provenance + receipt; CLI only annotates and verifies.
---
<!-- markdownlint-disable MD013 -->
# Generated option table, provenance and `--verify-provenance` (ADR-2044, ADR-2073)

- `cli_parse.cpp` includes `cli_options.gen.inc` (short string, `ARG_*`, `long_opts[]`, usage lines) inside its anon namespace; generated from option groups of `core/api/vmafx.toml`. New flag / alias / help text = definition entry + `python3 scripts/codegen/vmafx-api.py --write`, never an `.inc` edit. Existing spellings stay (HISS-14): `core/test/test_cli_option_table.cpp` frozen list.
- `--help` must keep `--no_cuda` / `--no_sycl` / `--no_hip` / `--no_metal` lines: both MCP servers probe `vmaf --help` for them.
- Report = `cli_write_report()` -> `vmafx_report_write()`: library writes `score_format`, `provenance`, `feature_backends`, `backend_used` (JSON), `<provenance>` (XML), sidecar on `--provenance-sidecar`. No post-write file edit in CLI (RC4 WP5 removed splice).
- vmafx calls live in C TU `cli_provenance.c`: C++ TU including generated C headers trips `modernize-use-using` / `performance-enum-size` (requests/WP1-5).
- `cli_annotate_run()` records argv minus output options (`cli_arg_is_output()`: -o/--output, --xml/--json/--csv/--sub, -q/--quiet, --provenance-sidecar, --verify-provenance) as `cli_argv` annotations. New output-only option -> add to `output_options[]`, else re-run with another output path never verifies.
- `--verify-provenance`: `cli_verify_provenance_arg()` scans argv before `cli_parse()`; re-run = `vmaf_cli_run()` with `cli_parse_reset()` (getopt state). Re-run report `<report>.rerun.json`, kept on mismatch. Exit 0 / 1 / 2.
