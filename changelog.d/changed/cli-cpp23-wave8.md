## CLI C++23 Wave 8 — `cli_parse.c` and `vmaf.c` converted to `.cpp` (ADR-0809)

`core/tools/cli_parse.c` and `core/tools/vmaf.c` are now compiled as C++23 translation
units. Conservative idioms only: `nullptr`, `static_cast`, `[[nodiscard]]`,
`[[noreturn]]`, and `std::string_view` for option-string comparisons. A `ModelArrays`
RAII struct in `vmaf.cpp` replaces the manual `vmaf_model_destroy` / free loops in the
goto-cleanup block, eliminating a class of potential leak on future early-return paths.
`cli_parse.h` gains `extern "C"` guards; `spinner.h` gains `static` internal linkage.
No user-visible functional change; `vmaf --help` and Netflix golden scores are identical.
