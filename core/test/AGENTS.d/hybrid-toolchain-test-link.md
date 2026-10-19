---
paths:
  - core/test/meson.build
invariant: With SYCL on, every test executable links as C++ via kwargs test_link_kwargs.
---
<!-- markdownlint-disable MD013 -->
# Test executables link as C++ when SYCL is on (ADR-1714)

`core/test/meson.build` defines `test_link_kwargs = is_sycl_enabled ?
{'link_language' : 'cpp'} : {}` at the top. Every `executable()` in that file
that does not set `link_language` itself passes `kwargs : test_link_kwargs`.
Why: `sycl_dependency` carries `-fsycl` and the AOT target flags in its link
arguments (ADR-1099, ADR-1360), which only the icpx driver accepts. With
`CC=gcc CXX=icpx` (the dev container and `Containerfile.vmafx`, ADR-1714) a
C-only test linked by GCC fails with `unrecognized command-line option
'-fsycl'`. On rebase: every executable master added since needs the kwargs
too; a test that sets `link_language` keeps its own value.
