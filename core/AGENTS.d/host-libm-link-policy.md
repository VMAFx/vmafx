---
paths:
  - core/src/meson.build
  - core/test/test_icx_system_libm.py
invariant: Every icx / icpx link gets -no-intel-lib=libimf; host math comes from glibc libm.
---
<!-- markdownlint-disable MD013 MD060 -->
# Host math library link policy (ADR-1495)

## Rebase-sensitive invariants

- **icx / icpx link glibc libm (ADR-1495)**: `core/src/meson.build` holds
  `BEGIN/END VMAF host math library link policy` block directly after strict
  FP policy, above first build target, then two
  `add_project_link_arguments(vmaf_c_host_libm_link_args / vmaf_cpp_..., language : 'c' / 'cpp')`
  lines. `intel-llvm` compiler -> `['-no-intel-lib=libimf']`, every other id
  -> `[]`. Why: Intel driver appends `-limf -lm`, rewrites given `-lm` into
  `-limf -lm`; icx executables take libimf statically (`-static-intel`
  default) and export math functions libvmaf.so imports -> CPU scores
  != GCC build (`log10`, `pow`, `powf`, `log2f`, `exp`, `log`). On rebase:
  keep block + both lines above first target (Meson refuses later); never
  add `-limf`, `find_library('imf')`, `-shared-intel` as substitute, or
  per-target link list that re-adds Intel math. Host link only: SYCL device
  code unaffected. Windows `icx-cl` not covered. Guards:
  `core/test/test_icx_system_libm.py` (readelf + `LD_DEBUG=bindings` on
  build's own libvmaf.so and vmaf; skips on non-icx builds). Its loader
  trace = list mode (`LD_TRACE_LOADED_OBJECTS=1 LD_WARN=1 LD_BIND_NOW=1`,
  nothing runs, glibc relocates with `__RTLD_NOIFUNC`); never `LD_BIND_NOW=1`
  on real run: Intel libimf.so binds `cosf` to libm's IFUNC without
  DT_NEEDED libm -> signal 11 on Ubuntu glibc (`LoaderTraceTest`),
  `test_strict_fp_compiler_args` (executes block per compiler pair, pins
  link lines).
