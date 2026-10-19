- **A compiler or linker warning now fails the Windows MSVC legs that print none.** `Windows MSVC+CUDA`,
  `Windows ARM64 MSVC` and `Windows MSVC+CUDA (full)` configure with `scripts/ci/werror-args.sh msvc`
  (`-Dwerror=true`: `/WX` on every `cl.exe` compile, `-WX` on every `link.exe` link, `--Werror
  all-warnings` on every nvcc fatbin). The icx-cl leg (`Windows MSVC+SYCL`) is not at zero yet and is
  listed with its count in [the CI overview](docs/development/ci.md#warnings-are-errors-adr-2170). See
  [ADR-2170](docs/adr/2170-warnings-are-errors-per-leg.md).
