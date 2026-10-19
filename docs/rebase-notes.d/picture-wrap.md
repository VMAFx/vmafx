## `vmaf_picture_wrap` port, Netflix/vmaf 700124a4c, ADR-2949 (2026-10-09)

- `core/include/libvmaf/picture.h`: `VmafPictureWrapped` and
  `vmaf_picture_wrap()` as upstream declares them, with `VMAF_DEPRECATED` /
  `VMAF_EXPORT` and one member per line (`w`, `h`). Upstream's declaration in
  `src/picture.h` is not taken: the public header is the one declaration.
- `core/src/picture.c`: upstream's `vmaf_picture_wrap()` split into
  `vmaf_picture_wrap_bind()` (construction, no checks, `*pic` untouched on
  failure, declared in `src/picture.h`) and the checked public body. **On
  sync**: keep `vmaf_picture_plane_extents()` (chroma rounded up, ADR-1483),
  the plane checks and the absence of `goto`; upstream's floor shifts and its
  `free_priv` label must not come back.
- `core/src/vmafx/frame_host.c`: `vmafx_frame_bind()` calls
  `vmaf_picture_wrap_bind()`; fork-only.
- `core/src/compat/libvmaf/picture.c`: the compat body on
  `vmafx_frame_wrap_host()` (`WrapRelease` thunk); fork-only.
- Generated: `core/api/vmafx.toml` `[[compat]]` entry, then
  `scripts/codegen/vmafx-api.py --write` (symbol list, engine names,
  conformance tables, compat reference).
- Tests: `test_picture_wrap_integration.c` is upstream's test, adapted to the
  public link (no private header) and to free the model; the fork's contract
  is `test_picture_wrap_api.c` and the `picture_wraps()` part of
  `test_compat_conformance_api.c`.
