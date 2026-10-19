## Windows MSVC+CUDA CI legs build without LTO (2026-10-13)

The `Windows MSVC+CUDA` row of `.github/workflows/libvmaf-build-matrix.yml` and
the `Windows MSVC+CUDA (full)` row of `.github/workflows/build.yml` pass
`-Db_lto=false`. Keep it when either file is rebased: with LTO each test program
links with `/LTCG` and the job runs into its time limit. No upstream file is
involved.
