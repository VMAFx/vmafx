- **Windows CLI Unicode paths**: `vmaf` and `vmafx` now receive the Unicode
  command line through `wmain` and convert arguments to strict UTF-8 before
  parsing, so accented and CJK input, output, and model paths no longer depend
  on the active ANSI code page.
