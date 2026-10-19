## libvmaf compat library on libvmafx: engine names, split library targets

`rc4/api-wp6-compat`, [ADR-1852](adr/1852-vmafx-api-redesign.md) decision D3,
[ADR-2094](adr/2094-libvmaf-compat-library-split.md).

- The library target is now `libvmafx` (`libvmafx.so.1`: the engine and the
  VMAFx API) plus the compat targets `libvmaf_shared_lib` /
  `libvmaf_static_lib` (`libvmaf.so.3`, sources `core/src/compat/libvmaf/`). A sync that adds a
  source to the old `libvmaf_sources` list adds it to `libvmafx_sources`; a
  sync that changes the library's link arguments changes `libvmafx`'s.
- Every engine translation unit is compiled with the generated
  `core/src/vmafx/engine_names_gen.h` (`add_project_arguments`), which
  renames the engine's own definition of each libvmaf function to
  `vmaf_engine_<stem>`. An upstream change to a libvmaf function body ports
  into the engine source as is (`core/src/libvmaf.c`, `model.c`, `picture.c`,
  `picture_v2.c`, `picture_convert.c`, `dict.cpp`, `dnn/`, `mcp/`, the HIP /
  Metal stubs); the 24 bodies in `core/src/libvmaf.c` that WP2 already
  spelled `vmaf_engine_<stem>` (`vmaf_engine_use_feature` and others) take
  the change under that name. Never add a definition of a libvmaf name to the engine and
  never compile an engine source without the header. A libvmaf function
  upstream adds needs a `[[compat]]` entry in `core/api/vmafx.toml` (the
  export checks fail otherwise) and a conformance call in
  `core/test/test_compat_conformance_*.c` (the coverage rule fails otherwise).
- Tests: `link_with : get_option('default_library') == 'both' ?
  libvmaf.get_static_lib() : libvmaf` is now `vmaf_test_link` (white-box,
  engine names) and `libvmaf_public_link` (black-box, needs
  `vmaf_public_name_args` in `c_args` and, for C++, `cpp_args`). Resolve a
  conflict in `core/test/meson.build` per hunk and keep the variable names.
- The WP2 forwarders at the end of `core/src/libvmaf.c` are gone; the
  `VmafModel` / `VmafModelCollection` structs gain `api_owner`.
- `vmaf_engine_read_pictures()` clears both caller `VmafPicture` structs once
  the context owns the pictures, in every build (a CUDA build left them
  pointing at released host translations; `test_compat_conformance` compares
  the traces). The body sits in `read_pictures_owned()`; an upstream change to
  `vmaf_read_pictures()` goes there and keeps the clearing in the wrapper.
- `core/include/libvmaf/*.h`: every exported declaration carries
  `VMAF_DEPRECATED("use <vmafx successor>")` (empty unless
  `VMAF_ENABLE_DEPRECATION_WARNINGS`); `test_libvmaf_deprecation` checks each
  marker against the definition. An upstream header sync keeps the markers.
- No score impact: the golden gate passes through the compat library and
  `test_compat_conformance` compares every compat function with its engine
  body; libvmaf return values are unchanged except the differences listed in
  `docs/api/vmafx/index.md`. No FFmpeg patch impact: unpatched FFmpeg n9.0.2
  builds against the split library and scores identically.
- The two libvmaf functions master gained after this branch's base are
  compat functions: `vmaf_set_sample_range_check_enabled()` sets the context
  option `check_sample_range`, `vmaf_set_input_colorimetry()` calls
  `vmafx_context_set_default_color()`. `vmafx_submit()` hands every pair's
  colour to the engine (`vmaf_engine_set_pair_colorimetry()`, which compares
  with `vmaf_conversion_policy_color_equal()` before
  `vmaf_conversion_state_set_input_color()` in
  `core/src/conversion_context.c`). An upstream change to the conversion
  state keeps that comparison: the same colour after the first converted
  pair is 0, another one `-EBUSY`.
