<!-- markdownlint-disable MD013 MD041 -->
- Fixed active GitHub code-scanning alerts for issue #1243:
  - `core/tools/cli_parse.cpp`: converted `for` loops that modified loop counters in their bodies (`cli_split` and `cli_unescape`) to idiomatic `while` loops (CodeQL `cpp/loop-variable-changed`, alerts 1030 and 1031).
  - `core/test/test_model_feature_overload_ownership.c`: removed terminal semicolon in comment to avoid false-positive statement detection (CodeQL `cpp/commented-out-code`, alert 951).
  - `mcp-server/vmaf-mcp/src/vmaf_mcp/server.py`: dropped redundant duplicate constants block containing unused `_VALID_AOM_CTCS` and `_VALID_NFLX_CTCS` and consolidated `_VALID_OUTPUT_FMTS` into canonical constants section (CodeQL `py/unused-global-variable`, alerts 970 and 971).
  - Audited remaining open alerts against `origin/master`, verifying that alert 1005 is load-bearing under ADR-0138 and cataloguing candidates for maintainer dismissal.
