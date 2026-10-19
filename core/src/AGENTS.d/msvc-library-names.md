---
paths:
  - core/src/meson.build
  - scripts/ci/check_msvc_library_names.py
invariant: Static MSVC-like builds install vmaf.lib / vmafx.lib via vmaf_static_name_kwargs; other builds keep Meson names.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Installed library names of MSVC builds (ADR-2752)

- `vmaf_static_name_kwargs` (`name_prefix: ''`, `name_suffix: 'lib'`) on
  both installed libraries: `libvmafx = library('vmafx', ...)` and the compat
  `static_library('vmaf', ...)`. Condition: `cc.get_argument_syntax() ==
  'msvc'` and `default_library == 'static'`. `both` keeps classic static
  names (no collision with the `vmaf.dll` import library).
- Not Meson's `namingscheme` option: needs Meson 1.10, floor is 1.4.0.
  Raise the floor -> may switch to it.
- **On upstream sync** (Netflix/vmaf `3b4dd350e`): do not import upstream's
  `shared_library()` + `static_library()` split or `vmaf-static.lib`; keep the
  keywords on both targets. Guard: `scripts/ci/check_msvc_library_names.py`
  on the MSVC CUDA / SYCL / ARM64 legs (files + pkg-config).
