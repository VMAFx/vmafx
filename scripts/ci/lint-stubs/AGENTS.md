<!-- markdownlint-disable MD013 MD032 MD060 -->
# `scripts/ci/lint-stubs/` — lint-only stand-in headers (ADR-2062)

`mex.h` and `matrix.h` under `scripts/ci/lint-stubs/matlab/` declare only what ten sources of
`compat/python-vmaf/matlab/` call. source that needs another MATLAB function adds its declaration
there (from documented signature, never MathWorks text); `test_gen_mex_compile_commands.py`
compiles every MEX source against stubs and fails when one stops parsing. generator runs in
`cpu` lane (`TIDY_RATCHET_COMPDB_cpu`) and in `Generate compile_commands.json` step of
changed-files job; it exits 1 with no sources or no stub, so lane cannot go clean by measuring
nothing. `mexErrMsgTxt` stays `_Noreturn`. Do not add exception or `exclude_untidyable()`
entry for these files, and do not put stub directory on any meson include path.
