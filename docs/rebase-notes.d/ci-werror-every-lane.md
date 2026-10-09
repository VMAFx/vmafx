## Warnings as errors on every CI lane (2026-10-09)

`ci/hiss10-werror-literal`, [ADR-2828](adr/2828-ci-werror-every-lane.md). A rebase or sync keeps
`-Dwerror=true` on every `meson setup` of `.github/workflows/` (on the first line of the `cmd`
legs), beside `scripts/ci/werror-args.sh`; `-DCMAKE_COMPILE_WARNING_AS_ERROR=ON` on the Level Zero
configures; `CGO_CFLAGS` with `-Werror` on `go-ci.yml`; `RUSTFLAGS: -D warnings` on `rust-ci.yml`;
`werror: msvc` on the Windows MSVC+SYCL row; and an empty `.config/lint-exceptions.d/HISS-10.toml`.
A new or rebased lane without its switch fails `praetorctl audit`. `write-compile-commands.py`
drops `-Werror` from the analysis database only (`test_write_compile_commands.py`). For a static
MSVC build `core/src/meson.build` passes the pkg-config module `-L${vmafx_libdir} -lvmafx` /
`-lvmaf` strings, not the renamed targets (the module warns on any `name_prefix` / `name_suffix`
target); keep that branch when an upstream sync touches the library naming, and
`check_msvc_library_names.py --meson-log` fails the warning on the MSVC legs.
