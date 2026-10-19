## icx-cl and the Windows icpx: strict FP without the override warning (2026-10-07)

`build/icx-cl-strict-fp-spelling`, [ADR-2170](adr/2170-warnings-are-errors-per-leg.md),
[Research-2170](research/2170-windows-strict-fp-spelling-2026-10-07.md). Fork-only: the `intel-llvm-cl` branch of the strict FP policy in
`core/src/meson.build` is `/fp:precise /clang:-fno-fast-math /clang:-fcomplex-arithmetic=full /clang:-ffp-contract=off` (it was `/fp:precise
/Qfma-`), and the SYCL policy gives `sycl_msvc_device_link` builds the same `-fno-fast-math -fcomplex-arithmetic=full` reset as Linux. A sync
keeps the order (model first, contraction-off last). no upstream file.
