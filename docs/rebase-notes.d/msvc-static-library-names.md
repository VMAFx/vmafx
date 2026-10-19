## Netflix/vmaf 3b4dd350e: static MSVC builds install vmaf.lib / vmafx.lib (2026-10-08)

- `core/src/meson.build`: `vmaf_static_name_kwargs` (`name_prefix: ''`,
  `name_suffix: 'lib'`) on `libvmafx = library('vmafx', ...)` and on the
  compat `static_library('vmaf', ...)`, for `cc.get_argument_syntax() ==
  'msvc'` with `default_library=static` only. Upstream splits its `library()`
  into `shared_library()` + `static_library()` and names the `both` static half
  `vmaf-static.lib`; the fork keeps its `library()` (ADR-2752). **On sync**:
  do not import that split; keep the keywords on both targets.
- `.github/workflows/libvmaf-build-matrix.yml`: the MSVC CUDA / SYCL and ARM64
  legs run `scripts/ci/check_msvc_library_names.py --prefix install` after
  `ninja install`.
