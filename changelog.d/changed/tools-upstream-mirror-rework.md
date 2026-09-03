- Rework upstream-mirror CLI and tool translation units (`cli_parse.cpp`,
  `y4m_input.c`, `vmaf.cpp`, `vmaf_bench.c`, `cli_parse.h`) to the
  fork lint profile (ADR-1155), reducing clang-tidy warnings from 348 to 0.
  Resolves dead twin `core/tools/cli_parse.c` under ADR-1153 precedent by
  deleting it and rewiring test harnesses to `cli_parse.cpp`.
  Preserves `NULL` in C translation units under ADR-1138 for MSVC `/std:clatest`
  compatibility. Modularizes option parsing into focused helpers, encapsulates
  `ModelArrays` members with RAII accessors, converts structs to designated
  initializers, eliminates integer multiplication widening in `y4m_input.c`,
  and preserves 100% bit-exact CLI output and exit code parity against pristine master.
