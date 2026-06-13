Fix Windows CI breakage in `core/test/test_output.c` introduced by the
`vmaf_write_output` / `vmaf_write_output_with_format` dispatcher tests
(PR #963). The two new test cases hard-coded a POSIX `mkstemp("/tmp/...")`
template and an unguarded `<unistd.h>` include, which fails to build on
MSVC (no `unistd.h`, no `mkstemp`) and fails at runtime on MinGW64
(MSYS2 runners do not expose a writable `/tmp` in the POSIX sense).
Introduce a portable `make_temp_path()` helper that uses `mkstemp` against
`$TMPDIR` on POSIX and `GetTempPathA` + `GetTempFileNameA` on Windows,
swap the two call sites to use it, guard the `<unistd.h>` include behind
`_WIN32`, and replace `unlink()` with the C-standard `remove()` so the
test compiles cleanly on all three Windows CI legs (MinGW64 CPU,
MSVC + CUDA, MSVC + oneAPI SYCL).
