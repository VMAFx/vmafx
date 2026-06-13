Removed duplicate `setenv`/`unsetenv` shim block in
`core/test/test_gpu_dispatch_runtime.c` that caused MinGW64 builds to fail
with `error: redefinition of 'test_setenv'`. The second `#if defined(_WIN32)`
block was redundant; the first `#ifdef _WIN32` block already installed
`test_setenv`/`test_unsetenv` wrappers and mapped the POSIX names to them via
preprocessor macros. (PR #744)
