### Fixed

`Build — Windows MSVC + CUDA` CI leg failed at configure time with meson error
"None of values ['c++23'] are supported by the CPP compiler" because meson's
MSVC backend does not include `c++23` in its accepted cpp_std values list.

Fix (ADR-1056): remove `cpp_std=c++23` from `default_options` in
`core/meson.build` and instead select the standard via
`add_project_arguments`, guarded by `get_option('cpp_std') == 'none'`.  MSVC
receives `/std:c++latest`; all other compilers receive `-std=c++23`.  The SYCL
leg's existing `-Dcpp_std=c++14` override is unaffected by the guard.
